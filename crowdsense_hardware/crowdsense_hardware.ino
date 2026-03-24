#include <Wire.h>
#include <vl53l8cx.h> // New ToF Library
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>

// --- Pin Configuration ---
#define I2C_SDA 21
#define I2C_SCL 22
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

#define ONE_WIRE_BUS 4
#define FLAME_DIGITAL 5
#define FLAME_ANALOG 34
#define GAS_DIGITAL 33
#define GAS_ANALOG 35

// --- Object Initialization ---
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
VL53L8CX sensor(&Wire, -1); // Replaced VL53L1X with VL53L8CX
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// --- Global Variables ---
bool tofSuccess = false;

// People Counting Variables
const int PERSON_THRESHOLD_MM = 1500; 
int totalInside = 0;
int totalEntries = 0;
int totalExits = 0;
int currentState = 0; 

// Environmental Variables (stored so OLED can refresh them without re-reading)
float currentTempC = 0.0;
int currentGasValue = 0;
int currentFlameValue = 0;
unsigned long lastEnvReadTime = 0; // For non-blocking 1-second timer

void setup() {
  Serial.begin(115200);
  delay(1000); // Give serial time to stabilize
  Serial.println("\n--- System Booting ---");

  // 1. Initialize I2C
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000); // Push I2C speed to 400kHz for the VL53L8CX

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

  // 3. Initialize VL53L8CX (Multizone)
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

  // 6. Setup WiFi Access Point
  WiFi.softAP("ESP32_CrowdSense", "12345678");
  Serial.println("WiFi AP Ready.");
  
  Serial.println("--- Setup Complete ---");
}

void loop() {
  // =========================================================
  // TASK 1: READ ENVIRONMENTAL SENSORS (ONCE PER SECOND)
  // =========================================================
  // We use millis() instead of delay(1000) so we don't block the ToF sensor!
  if (millis() - lastEnvReadTime >= 1000) {
    lastEnvReadTime = millis();
    
    currentFlameValue = analogRead(FLAME_ANALOG);
    currentGasValue = analogRead(GAS_ANALOG);
    
    sensors.requestTemperatures();
    currentTempC = sensors.getTempCByIndex(0);

    // Output to Serial periodically
    Serial.print("Temp: "); Serial.print(currentTempC); Serial.print("C | ");
    Serial.print("Gas: "); Serial.print(currentGasValue); Serial.print(" | ");
    Serial.print("Flame: "); Serial.print(currentFlameValue); Serial.print(" | ");
    Serial.print("People Inside: "); Serial.println(totalInside);
  }

  // =========================================================
  // TASK 2: PROCESS MULTIZONE ToF DATA (CONTINUOUSLY)
  // =========================================================
  if (tofSuccess) {
    VL53L8CX_ResultsData results;
    uint8_t dataReady = 0;

    sensor.check_data_ready(&dataReady);

    if (dataReady) {
      sensor.get_ranging_data(&results);

      bool zoneA_active = false; // "Front" half
      bool zoneB_active = false; // "Back" half

      for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
          int i = x + (y * 4); 
          int distance = results.distance_mm[i];
          uint8_t status = results.target_status[i];

          if ((status == 5 || status == 6 || status == 9) && distance > 0 && distance < PERSON_THRESHOLD_MM) {
            if (y < 2) zoneA_active = true;
            else zoneB_active = true;
          }
        }
      }

      // --- State Machine Logic ---
      if (currentState == 0) {
        if (zoneA_active && !zoneB_active) currentState = 1; 
        if (!zoneA_active && zoneB_active) currentState = 3; 
      }
      else if (currentState == 1) {
        if (zoneA_active && zoneB_active) currentState = 2; 
        if (!zoneA_active && zoneB_active) currentState = 3; 
        if (!zoneA_active && !zoneB_active) currentState = 0; 
      }
      else if (currentState == 2) {
        if (!zoneA_active && zoneB_active) currentState = 3; 
        if (zoneA_active && !zoneB_active) currentState = 1; 
        if (!zoneA_active && !zoneB_active) currentState = 0; 
      }
      else if (currentState == 3) {
        if (!zoneA_active && !zoneB_active) {
          totalEntries++;
          totalInside++;
          currentState = 0; 
        }
        if (zoneA_active && zoneB_active) currentState = 2; 
        if (zoneA_active && !zoneB_active) {
          totalExits++;
          totalInside--;
          currentState = 0; 
        }
      }

      if (totalInside < 0) totalInside = 0;

      // =========================================================
      // TASK 3: UPDATE OLED UI WITH ALL DATA
      // =========================================================
      display.clearDisplay();
      
      // Top Row: Status
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("CrowdSense AP Active");
      
      // Sensor Status Indicators (Top Right)
      display.fillRect(105, 0, 8, 8, zoneA_active ? SSD1306_WHITE : SSD1306_BLACK);
      display.fillRect(115, 0, 8, 8, zoneB_active ? SSD1306_WHITE : SSD1306_BLACK);

      // Middle Row: People Count
      display.setTextSize(2);
      display.setCursor(0, 15);
      display.print("IN: ");
      display.print(totalInside);

      // Environmental Data
      display.setTextSize(1);
      display.setCursor(0, 35);
      display.print("T:"); display.print(currentTempC, 1); display.print("C ");
      display.print("G:"); display.print(currentGasValue); display.print(" ");
      display.print("F:"); display.print(currentFlameValue);
      
      // Bottom Row: Stats
      display.setCursor(0, 50);
      display.print("Tot In:"); display.print(totalEntries);
      display.print(" Out:"); display.print(totalExits);

      display.display();
    }
  }
}