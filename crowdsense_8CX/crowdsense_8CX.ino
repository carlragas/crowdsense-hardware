#include <Wire.h>
#include <vl53l8cx.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFiManager.h>
#include <Firebase_ESP_Client.h>

// Provide the token generation process info
#include <addons/TokenHelper.h>

// ============ CONFIGURATION ============
#define I2C_SDA 21
#define I2C_SCL 22

#define ONE_WIRE_BUS 4
#define BACKUP_FLAME_DIGITAL 5
#define MAIN_FLAME 14
#define SIREN_1 19
#define SIREN_2 18
#define BACKUP_FLAME_ANALOG 34
#define GAS_DIGITAL 33
#define GAS_ANALOG 35

// Firebase Configuration
#define FIREBASE_HOST "https://crowdsense-db-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define FIREBASE_LEGACY_TOKEN "5mGeiwSA9PLndbFmJZtC8x7a9U78VaM0H21nh1nd"

// Data send interval (milliseconds)
const unsigned long FIREBASE_SEND_INTERVAL = 2000; // Send data every 2 seconds
unsigned long lastFirebaseSendTime = 0;

// --- Object Initialization ---
VL53L8CX sensor(&Wire, -1);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// --- Global Variables ---
bool tofSuccess = false;
String deviceMAC = "D4:E9:F4:FA:DF:5C";

// People Counting Variables
const int PERSON_THRESHOLD_MM = 1500;
int totalInside = 0;
int totalEntries = 0;
int totalExits = 0;

// MULTI-LANE TRACKING
int laneState[4] = {0, 0, 0, 0};

// Cooldown timers
unsigned long lastEntryTime = 0;
unsigned long lastExitTime = 0;
const int EVENT_COOLDOWN_MS = 800;

// Environmental Variables
float currentTempC = 0.0;
int currentGasValue = 0;
int currentBackupFlameValue = 0;
int currentMainFlameValue = 0;
unsigned long lastEnvReadTime = 0;

// Firebase connection status
bool firebaseConnected = false;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- System Booting ---");
  
  // Initialize I2C
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  // Initialize Pins
  pinMode(BACKUP_FLAME_DIGITAL, INPUT);
  pinMode(MAIN_FLAME, INPUT_PULLUP);
  pinMode(GAS_DIGITAL, INPUT);
  pinMode(SIREN_1, OUTPUT);
  pinMode(SIREN_2, OUTPUT);
  digitalWrite(SIREN_1, LOW);
  digitalWrite(SIREN_2, LOW);
  Serial.println("Pins Initialized");

  // Initialize VL53L8CX
  sensor.begin();
  sensor.off();
  sensor.on();
  Serial.println("ToF ON");
  
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

  // Initialize DS18B20
  sensors.begin();
  Serial.println("DS18B20 Initialized.");

  // WiFi and Firebase Setup
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.resetSettings();
  
  Serial.println("Connecting to WiFi...");
  bool res = wm.autoConnect("CrowdSense_Parking", "12345678");
  
  if (!res) {
    Serial.println("Failed to establish WiFi connection.");
    firebaseConnected = false;
  } else {
    Serial.println("WiFi connected successfully.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    
    // Configure Firebase
    config.database_url = FIREBASE_HOST;
    config.signer.tokens.legacy_token = FIREBASE_LEGACY_TOKEN;
    
    // Assign the callback function for token generation
    config.token_status_callback = tokenStatusCallback;
    
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);
    
    // Test Firebase connection
    Serial.println("Testing Firebase connection...");
    if (Firebase.ready()) {
      firebaseConnected = true;
      Serial.println("Firebase connected successfully!");
      
      // Send initial device status
      String deviceStatusPath = "/sensor_data/" + deviceMAC + "/status";
      Firebase.RTDB.setString(&fbdo, deviceStatusPath.c_str(), "online");
      Firebase.RTDB.setInt(&fbdo, "/sensor_data/" + deviceMAC + "/timestamp", millis());
    } else {
      Serial.println("Firebase connection failed!");
      firebaseConnected = false;
    }
  }
  
  Serial.println("--- Setup Complete ---");
}

void loop() {
  // =========================================================
  // TASK 1: READ ENVIRONMENTAL SENSORS (ONCE PER SECOND)
  // =========================================================
  if (millis() - lastEnvReadTime >= 1000) {
    lastEnvReadTime = millis();
    
    currentBackupFlameValue = analogRead(BACKUP_FLAME_ANALOG);
    currentGasValue = analogRead(GAS_ANALOG);
    
    sensors.requestTemperatures();
    currentTempC = sensors.getTempCByIndex(0);
    
    // Convert analog gas value to percentage (adjust min/max as needed)
    int gasPercentage = map(currentGasValue, 0, 4095, 0, 100);
    int flamePercentage = map(currentBackupFlameValue, 0, 4095, 0, 100);
    
    Serial.print("Temp: "); Serial.print(currentTempC); Serial.print("C | ");
    Serial.print("Gas: "); Serial.print(currentGasValue); Serial.print(" ("); Serial.print(gasPercentage); Serial.print("%) | ");
    Serial.print("Flame: "); Serial.print(currentBackupFlameValue); Serial.print(" ("); Serial.print(flamePercentage); Serial.print("%) | ");
    Serial.print("People Inside: "); Serial.println(totalInside);
  }
  
  // Siren control logic
  if (currentBackupFlameValue <= 1000 && currentGasValue >= 500) {
    digitalWrite(SIREN_2, HIGH);
  } else {
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
      
      bool laneA[4] = {false, false, false, false};
      bool laneB[4] = {false, false, false, false};
      
      for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
          int i = x + (y * 4);
          int distance = results.distance_mm[i];
          uint8_t status = results.target_status[i];
          
          if ((status == 5 || status == 6 || status == 9) && distance > 0 && distance < PERSON_THRESHOLD_MM) {
            if (y < 2) laneA[x] = true;
            else laneB[x] = true;
          }
        }
      }
      
      unsigned long currentMillis = millis();
      
      for (int x = 0; x < 4; x++) {
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
    }
  }
  
  // =========================================================
  // TASK 3: SEND DATA TO FIREBASE
  // =========================================================
  if (firebaseConnected && (millis() - lastFirebaseSendTime >= FIREBASE_SEND_INTERVAL)) {
    lastFirebaseSendTime = millis();
    
    // Check if Firebase is ready
    if (Firebase.ready()) {
      String basePath = "/sensor_data/" + deviceMAC + "/";
      
      // Send Temperature
      String tempPath = basePath + "temperature";
      if (Firebase.RTDB.setFloat(&fbdo, tempPath.c_str(), currentTempC)) {
        Serial.print("✓ Temperature sent: ");
        Serial.println(currentTempC);
      } else {
        Serial.print("✗ Temperature send failed: ");
        Serial.println(fbdo.errorReason());
      }
      
      // Send Gas value (analog and percentage)
      String gasPath = basePath + "gas_analog";
      if (Firebase.RTDB.setInt(&fbdo, gasPath.c_str(), currentGasValue)) {
        Serial.print("✓ Gas analog sent: ");
        Serial.println(currentGasValue);
      } else {
        Serial.print("✗ Gas analog send failed: ");
        Serial.println(fbdo.errorReason());
      }
      
      // Send Gas percentage
      String gasPercentPath = basePath + "gas_percentage";
      int gasPercentage = map(currentGasValue, 0, 4095, 0, 100);
      Firebase.RTDB.setInt(&fbdo, gasPercentPath.c_str(), gasPercentage);
      
      // Send Flame value
      String flamePath = basePath + "flame_analog";
      if (Firebase.RTDB.setInt(&fbdo, flamePath.c_str(), currentBackupFlameValue)) {
        Serial.print("✓ Flame analog sent: ");
        Serial.println(currentBackupFlameValue);
      } else {
        Serial.print("✗ Flame analog send failed: ");
        Serial.println(fbdo.errorReason());
      }
      
      // Send People count data
      String peopleInsidePath = basePath + "people_inside";
      if (Firebase.RTDB.setInt(&fbdo, peopleInsidePath.c_str(), totalInside)) {
        Serial.print("✓ People inside sent: ");
        Serial.println(totalInside);
      }
      
      String entriesPath = basePath + "total_entries";
      Firebase.RTDB.setInt(&fbdo, entriesPath.c_str(), totalEntries);
      
      String exitsPath = basePath + "total_exits";
      Firebase.RTDB.setInt(&fbdo, exitsPath.c_str(), totalExits);
      
      // Send timestamp
      String timestampPath = basePath + "last_update";
      Firebase.RTDB.setInt(&fbdo, timestampPath.c_str(), millis());
      
      // Send flame and gas digital status (for alarms)
      String flameDigitalPath = basePath + "flame_detected";
      bool flameDetected = (currentBackupFlameValue <= 1000);
      Firebase.RTDB.setBool(&fbdo, flameDigitalPath.c_str(), flameDetected);
      
      String gasDigitalPath = basePath + "gas_detected";
      bool gasDetected = (currentGasValue >= 500);
      Firebase.RTDB.setBool(&fbdo, gasDigitalPath.c_str(), gasDetected);
      
      // Send siren status
      String sirenPath = basePath + "siren_active";
      bool sirenActive = (currentBackupFlameValue <= 1000 && currentGasValue >= 500);
      Firebase.RTDB.setBool(&fbdo, sirenPath.c_str(), sirenActive);
      
      Serial.println("--- Firebase data update complete ---");
      
    } else {
      Serial.println("Firebase not ready. Reconnecting...");
      // Attempt to reconnect
      Firebase.begin(&config, &auth);
      delay(100);
    }
  }
}


