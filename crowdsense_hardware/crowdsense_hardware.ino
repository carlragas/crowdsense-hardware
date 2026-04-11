#include <Wire.h>
#include <vl53l8cx.h> // New ToF Library
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
VL53L8CX sensor(&Wire, -1); 
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
const int EVENT_COOLDOWN_MS = 800; // Ignore duplicate events within 800ms

// Environmental Variables
float currentTempC = 0.0;
int currentGasValue = 0;
int currentFlameValue = 0;
unsigned long lastEnvReadTime = 0; 

void setup() {
  Serial.begin(115200);
  delay(1000); 
  Serial.println("\n--- System Booting ---");

  // 1. Initialize I2C
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

  // 3. Initialize VL53L8CX 
  display.println("Starting ToF...");
  display.display();
  
  sensor.begin();
  sensor.off();
  sensor.on();
  
  if (sensor.init() != 0) {
    Serial.println("CRITICAL: VL53L8CX sensor not found!");
    tofSuccess = false;
  } else {
    sensor.set_resolution(VL53L8CX_RESOLUTION_4X4);
    sensor.set_ranging_frequency_hz(15);
    sensor.start_ranging();
    tofSuccess = true;
    Serial.println("VL53L8CX Initialized.");
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
    }
  else {
    digitalWrite(SIREN_2, LOW);

  }

  // =========================================================
  // TASK 2: PROCESS MULTI-LANE ToF DATA (CONTINUOUSLY)
  // =========================================================
  if (tofSuccess) {
    VL53L8CX_ResultsData results;
    uint8_t dataReady = 0;

    sensor.check_data_ready(&dataReady);

    if (dataReady) {
      sensor.get_ranging_data(&results);

      // Arrays to track activity in each of the 4 columns
      bool laneA[4] = {false, false, false, false}; // Entry side of the lane
      bool laneB[4] = {false, false, false, false}; // Exit side of the lane

      // Map the 4x4 grid into our lanes
      for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
          int i = x + (y * 4); 
          int distance = results.distance_mm[i];
          uint8_t status = results.target_status[i];

          if ((status == 5 || status == 6 || status == 9) && distance > 0 && distance < PERSON_THRESHOLD_MM) {
            if (y < 2) laneA[x] = true; // Top two rows
            else laneB[x] = true;       // Bottom two rows
          }
        }
      }

      unsigned long currentMillis = millis();
      bool globalA = false; // Just for OLED indicators
      bool globalB = false;

      // --- Process 4 Independent State Machines ---
      for (int x = 0; x < 4; x++) {
        if (laneA[x]) globalA = true;
        if (laneB[x]) globalB = true;

        bool A = laneA[x];
        bool B = laneB[x];

        // States: 0=Empty, 1=EntryStart, 2=EntryMid, 3=EntryLeave
        //         4=ExitStart, 5=ExitMid, 6=ExitLeave
        switch(laneState[x]) {
          case 0: // Empty
            if (A && !B) laneState[x] = 1;
            else if (!A && B) laneState[x] = 4;
            break;
            
          case 1: // Entry Started
            if (A && B) laneState[x] = 2;
            else if (!A && B) laneState[x] = 3;
            else if (!A && !B) laneState[x] = 0;
            break;
            
          case 2: // Entry Middle
            if (!A && B) laneState[x] = 3;
            else if (A && !B) laneState[x] = 1;
            else if (!A && !B) laneState[x] = 0;
            break;
            
          case 3: // Entry Leaving
            if (!A && !B) {
              // Successfully walked through! Check cooldown to prevent double-counting
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

          case 4: // Exit Started
            if (A && B) laneState[x] = 5;
            else if (A && !B) laneState[x] = 6;
            else if (!A && !B) laneState[x] = 0;
            break;

          case 5: // Exit Middle
            if (A && !B) laneState[x] = 6;
            else if (!A && B) laneState[x] = 4;
            else if (!A && !B) laneState[x] = 0;
            break;

          case 6: // Exit Leaving
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
      
      // Global Activity Indicators
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