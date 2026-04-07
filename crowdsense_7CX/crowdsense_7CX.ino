#include <vl53l7cx_class.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>

// ============ CONFIGURATION ============
#define I2C_SDA 21
#define I2C_SCL 22
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

#define ONE_WIRE_BUS 4
#define FLAME_DIGITAL 5
#define SIREN_1 19
#define SIREN_2 18
#define FLAME_ANALOG 34
#define GAS_DIGITAL 33
#define GAS_ANALOG 35

// --- Object Initialization ---
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
// The library expects (Wire, LPN_PIN, RST_PIN). Using -1 for unused reset pins.
VL53L7CX sensor(&Wire, -1, -1); 
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// --- Global Variables ---
bool tofSuccess = false;

// People Counting Variables
const int PERSON_THRESHOLD_MM = 1500; 
int totalInside = 0;
int totalEntries = 0;
int totalExits = 0;

// MULTI-LANE TRACKING: 4 separate state machines for columns 0, 1, 2, and 3
int laneState[4] = {0, 0, 0, 0}; 

// Cooldown timers to prevent a single person triggering multiple lanes at once
unsigned long lastEntryTime = 0;
unsigned long lastExitTime = 0;
const int EVENT_COOLDOWN_MS = 800; 

// Environmental Variables
float currentTempC = 0.0;
int currentGasValue = 0;
int currentFlameValue = 0;
unsigned long lastEnvReadTime = 0; 

void setup() {
  Serial.begin(115200);
  delay(1000); 
  Serial.println("\n--- System Booting ---");

  // 1. Initialize I2C for ESP32
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000); 

  // 2. Initialize OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED Failed!");
  } else {
    display.clearDisplay();
    display.setTextColor(WHITE);
    display.setCursor(0,0);
    display.println("Booting System...");
    display.display();
  }

  // 3. Initialize VL53L7CX 
  display.println("Starting ToF...");
  display.display();
  
  sensor.begin(); // Setup I2C interface
  
  if (sensor.init_sensor() != 0) {
    Serial.println("CRITICAL: VL53L7CX sensor not found!");
    tofSuccess = false;
  } else {
    // STM32duino library specific methods
    sensor.vl53l7cx_set_resolution(VL53L7CX_RESOLUTION_4X4);
    sensor.vl53l7cx_set_ranging_frequency_hz(15);
    sensor.vl53l7cx_start_ranging();
    tofSuccess = true;
    Serial.println("VL53L7CX Initialized.");
  }

  // 4. Initialize DS18B20
  sensors.begin();
  Serial.println("DS18B20 Initialized.");

  // 5. Configure Digital Pins
  pinMode(FLAME_DIGITAL, INPUT);
  pinMode(GAS_DIGITAL, INPUT);
  pinMode(SIREN_1, OUTPUT);
  pinMode(SIREN_2, OUTPUT);
  digitalWrite(SIREN_1, LOW);
  digitalWrite(SIREN_2, LOW);

  // 6. Setup WiFi Access Point
  WiFi.softAP("ESP32_CrowdSense", "12345678");
  Serial.println("WiFi AP Ready.");
  
  Serial.println("--- Setup Complete ---");
}

void loop() {
  // =========================================================
  // TASK 1: READ ENVIRONMENTAL SENSORS (ONCE PER SECOND)
  // =========================================================
  if (millis() - lastEnvReadTime >= 1000) {
    lastEnvReadTime = millis();
    
    currentFlameValue = analogRead(FLAME_ANALOG);
    currentGasValue = analogRead(GAS_ANALOG);
    
    sensors.requestTemperatures();
    currentTempC = sensors.getTempCByIndex(0);

    Serial.print("Temp: "); Serial.print(currentTempC); Serial.print("C | ");
    Serial.print("Gas: "); Serial.print(currentGasValue); Serial.print(" | ");
    Serial.print("Flame: "); Serial.print(currentFlameValue); Serial.print(" | ");
    Serial.print("People Inside: "); Serial.println(totalInside);
  }

  if (currentFlameValue <= 1000 && currentGasValue >= 500) {
    digitalWrite(SIREN_2, HIGH);
  } else {
    digitalWrite(SIREN_2, LOW);
  }

  // =========================================================
  // TASK 2: PROCESS MULTI-LANE ToF DATA (CONTINUOUSLY)
  // =========================================================
  if (tofSuccess) {
    VL53L7CX_ResultsData results;
    uint8_t dataReady = 0;

    // Use the specific check function from the library
    sensor.vl53l7cx_check_data_ready(&dataReady);

    if (dataReady) {
      sensor.vl53l7cx_get_ranging_data(&results);

      bool laneA[4] = {false, false, false, false}; 
      bool laneB[4] = {false, false, false, false}; 

      for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
          int zone = x + (y * 4); 
          
          /** * For multi-target sensors, distance_mm is an array that accounts for targets per zone.
           * We pull the first target found in each zone.
           **/
          int targetIndex = zone * VL53L7CX_NB_TARGET_PER_ZONE;
          int distance = results.distance_mm[targetIndex];
          uint8_t status = results.target_status[targetIndex];

          if ((status == 5 || status == 6 || status == 9) && distance > 0 && distance < PERSON_THRESHOLD_MM) {
            if (y < 2) laneA[x] = true; 
            else laneB[x] = true;       
          }
        }
      }

      unsigned long currentMillis = millis();
      bool globalA = false; 
      bool globalB = false;

      for (int x = 0; x < 4; x++) {
        if (laneA[x]) globalA = true;
        if (laneB[x]) globalB = true;

        bool A = laneA[x];
        bool B = laneB[x];

        switch(laneState[x]) {
          case 0: 
            if (A && !B) laneState[x] = 1;
            else if (!A && B) laneState[x] = 4;
            break;
          case 1: 
            if (A && B) laneState[x] = 2;
            else if (!A && B) laneState[x] = 3;
            else if (!A && !B) laneState[x] = 0;
            break;
          case 2: 
            if (!A && B) laneState[x] = 3;
            else if (A && !B) laneState[x] = 1;
            else if (!A && !B) laneState[x] = 0;
            break;
          case 3: 
            if (!A && !B) {
              if (currentMillis - lastEntryTime > EVENT_COOLDOWN_MS) {
                totalEntries++;
                totalInside++;
                lastEntryTime = currentMillis;
              }
              laneState[x] = 0;
            }
            else if (A && B) laneState[x] = 2;
            else if (A && !B) laneState[x] = 1;
            break;
          case 4: 
            if (A && B) laneState[x] = 5;
            else if (A && !B) laneState[x] = 6;
            else if (!A && !B) laneState[x] = 0;
            break;
          case 5: 
            if (A && !B) laneState[x] = 6;
            else if (!A && B) laneState[x] = 4;
            else if (!A && !B) laneState[x] = 0;
            break;
          case 6: 
            if (!A && !B) {
              if (currentMillis - lastExitTime > EVENT_COOLDOWN_MS) {
                totalExits++;
                totalInside--;
                lastExitTime = currentMillis;
              }
              laneState[x] = 0;
            }
            else if (A && B) laneState[x] = 5;
            else if (!A && B) laneState[x] = 4;
            break;
        }
      }

      if (totalInside < 0) totalInside = 0;

      // =========================================================
      // TASK 3: UPDATE OLED UI 
      // =========================================================
      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("CrowdSense AP Active");
      display.fillRect(105, 0, 8, 8, globalA ? SSD1306_WHITE : SSD1306_BLACK);
      display.fillRect(115, 0, 8, 8, globalB ? SSD1306_WHITE : SSD1306_BLACK);
      display.setTextSize(2);
      display.setCursor(0, 15);
      display.print("IN: ");
      display.print(totalInside);
      display.setTextSize(1);
      display.setCursor(0, 35);
      display.print("T:"); display.print(currentTempC, 1); display.print("C ");
      display.print("G:"); display.print(currentGasValue); display.print(" ");
      display.print("F:"); display.print(currentFlameValue);
      display.setCursor(0, 50);
      display.print("Tot In:"); display.print(totalEntries);
      display.print(" Out:"); display.print(totalExits);
      display.display();
    }
  }
}