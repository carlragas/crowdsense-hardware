// Libraries
#include <Wire.h>
#include <vl53l8cx.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFiManager.h>
#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>

// Pin Configurations
#define ONE_WIRE_BUS 4
#define BACKUP_FLAME_DIGITAL 5
#define MAIN_FLAME 14
#define SIREN_2 25
#define SIREN_1 26
#define I2C_SDA 21 //switch SDA and SCL pin on final assembly
#define I2C_SCL 22
#define UPS_POWER_INDICATOR 32
#define GAS_DIGITAL 33
#define BACKUP_FLAME_ANALOG 34
#define GAS 35
#define FIREBASE_HOST "https://crowdsense-db-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define FIREBASE_LEGACY_TOKEN "5mGeiwSA9PLndbFmJZtC8x7a9U78VaM0H21nh1nd"

// Initializations
VL53L8CX sensor(&Wire, -1);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

//Database Variables
const unsigned long FIREBASE_SEND_INTERVAL = 2000; // Interval for sending data in the database
unsigned long lastFirebaseSendTime = 0;
bool firebaseConnected = false;
String deviceMAC = "00:00:00:00:00:00";
// ToF Variables
bool tofSuccess = false;
const int PERSON_THRESHOLD_MM = 1500;
int totalInside = 0;
int totalEntries = 0;
int totalExits = 0;
//ToF Variable: MULTI-LANE TRACKING
int laneState[4] = {0, 0, 0, 0};
//ToF Variables: Cooldown timers
unsigned long lastEntryTime = 0;
unsigned long lastExitTime = 0;
const int EVENT_COOLDOWN_MS = 800;
// Environmental Variables
float currentTempC = 0.0;
int currentGasValue = 0;
bool currentMainFlameValue = true;
int currentBackupFlameValue = 0;
bool esp32Online = true;
unsigned long lastEnvReadTime = 0;
// Siren Variables
bool sirenAlertActive = false;
bool sirenClearActive = false;
unsigned long sirenAlertTimer = 0;
unsigned long sirenClearTimer = 0;
const unsigned long sirenAlertDuration = 5000; 
const unsigned long sirenClearDuration = 60000; 
// Power Variables - Voltage Divider
const unsigned long checkPowerInterval = 5000;
unsigned long lastPowerCheckedTime = 0;
const float Resistor1 = 10000.0;
const float Resistor2 = 3300.0;
const float powerRatio = (Resistor1 + Resistor2)/Resistor2;
const float upperPowerThreshold = 11.5;
const float lowerPowerThreshold = 10.8;
String powerStatus;

void getDeviceMAC(){
  WiFi.begin();
  deviceMAC = WiFi.macAddress();
  if (deviceMAC == "00:00:00:00:00:00"){
    Serial.println("ERROR: Cannot obtain device MAC Address.");
  }
  else if (deviceMAC != "00:00:00:00:00:00"){
    Serial.println("Device Mac Address: " + deviceMAC);
  }
}

void connectNetwork(){
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
    connectDB();
  }
}

void connectDB(){
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
    Firebase.RTDB.setString(&fbdo, deviceStatusPath.c_str(), esp32Online);
    Firebase.RTDB.setInt(&fbdo, "/sensor_data/" + deviceMAC + "/timestamp", millis());
    timeClient.begin();
    // Set offset time in seconds to adjust for your timezone 
    // GMT+8 (Philippines) = 8 * 60 * 60 = 28800
    timeClient.setTimeOffset(28800);
    } else {
      Serial.println("Firebase connection failed!");
      firebaseConnected = false;
    }
}

void checkPowerStatus(){
  if (firebaseConnected && (millis() - lastPowerCheckedTime >= checkPowerInterval)){
    lastPowerCheckedTime = millis();
    int rawPinReading = analogRead(UPS_POWER_INDICATOR);
    float pinVoltage = (rawPinReading / 4095.0)*3.3;
    float upsVoltage = pinVoltage * powerRatio;
    if (upsVoltage >= upperPowerThreshold) {
      powerStatus = "High";
    } 
    else if (upsVoltage < upperPowerThreshold && upsVoltage >= lowerPowerThreshold){
      powerStatus = "Adequate";
    } 
    else{
      powerStatus = "Low";
  }
  Serial.println("Power Status: " + powerStatus);
  }
}

void readEnvironment(){
  if (millis() - lastEnvReadTime >= 1000) {
  lastEnvReadTime = millis();
  currentBackupFlameValue = analogRead(BACKUP_FLAME_ANALOG);
  currentMainFlameValue = digitalRead(MAIN_FLAME);
  currentGasValue = analogRead(GAS);
    
  sensors.requestTemperatures();
  currentTempC = sensors.getTempCByIndex(0);
    Serial.print("Temp: "); Serial.print(currentTempC); Serial.print("C | ");
    Serial.print("Gas: "); Serial.print(currentGasValue); Serial.print(" | ");
    Serial.print("Flame: "); Serial.print(currentBackupFlameValue); Serial.print(" | ");
    Serial.print("People Inside: "); Serial.println(totalInside);
  }
}

void countCrowd(){
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
  
}

void triggerAlertSiren(){
  bool alertStatus = (!currentMainFlameValue || currentBackupFlameValue <= 1000) && (currentGasValue >= 600);
  if (alertStatus && !sirenClearActive) {
    if  (!sirenAlertActive){
      sirenAlertActive =  true;
      digitalWrite(SIREN_2,HIGH);
    }
    sirenAlertTimer = millis() + sirenAlertDuration;
    Serial.println("ALERT ON: Emergency Fire Siren Activated.");
  }

  if (sirenAlertActive && millis() >= sirenAlertTimer){
    sirenAlertActive = false;
    digitalWrite(SIREN_2, LOW);
    Serial.println("ALERT OFF: Emergency Fire Siren Deactivated.");
  }
}

void triggerClearSiren(){
  bool clearStatus = sirenAlertActive && totalInside == 0;
  if (clearStatus){
    if (!sirenClearActive){
      sirenClearActive = true;
      digitalWrite(SIREN_1, HIGH);
      sirenClearTimer = millis() + sirenClearDuration;
    }
    Serial.println("AREA CLEAR: All personnel have evacuated the premises.");
    clearStatus = false;
  }

  if (sirenClearActive && millis() >= sirenClearTimer) {
    sirenClearActive = false;
    digitalWrite(SIREN_1, LOW);
  }
}

void uploadData(){
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
      } 
      else {
        Serial.print("✗ Temperature send failed: ");
        Serial.println(fbdo.errorReason());
      }
      
      // Send Gas value (analog and percentage)
      String gasPath = basePath + "gas";
      if (Firebase.RTDB.setInt(&fbdo, gasPath.c_str(), currentGasValue)) {
        Serial.print("✓ Gas analog sent: ");
        Serial.println(currentGasValue);
      } else {
        Serial.print("✗ Gas analog send failed: ");
        Serial.println(fbdo.errorReason());
      }
      
      // Send Flame value
      String flamePath = basePath + "flame";
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
      
      timeClient.begin();
      // Set offset time in seconds to adjust for your timezone 
      // GMT+8 (Philippines) = 8 * 60 * 60 = 28800
      timeClient.setTimeOffset(28800);
      // Send timestamp
      unsigned long epochTime = timeClient.getEpochTime();
      // To get currentEpochMillis (Milliseconds)
      // Note: Most NTP libraries return seconds. We multiply by 1000 
      // and add the internal millis() remainder for precision.
      long long currentEpochMillis = ((long long)epochTime * 1000) + (millis() % 1000);
      String timestampPath = basePath + "last_updated";
      Firebase.RTDB.setInt(&fbdo, timestampPath.c_str(), currentEpochMillis);
  
      // Send siren status
      String sirenAlertPath = basePath + "siren_alert_active";
      Firebase.RTDB.setBool(&fbdo, sirenAlertPath.c_str(), sirenAlertActive);
      String sirenClearPath = basePath + "siren_clear_active";
      Firebase.RTDB.setBool(&fbdo, sirenClearPath.c_str(), sirenClearActive);

      // Send Power Status
      String powerStatusPath = basePath + "power_status";
      Firebase.RTDB.setString(&fbdo, powerStatusPath.c_str(), powerStatus);
      
      Serial.println("--- Firebase data update complete ---");
      
    } else {
      Serial.println("Firebase not ready. Reconnecting...");
      // Attempt to reconnect
      Firebase.begin(&config, &auth);
      delay(100);
    }
  }
}

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
  pinMode(UPS_POWER_INDICATOR, INPUT);
  pinMode(SIREN_1, OUTPUT);
  pinMode(SIREN_2, OUTPUT);
  digitalWrite(SIREN_1, LOW);
  digitalWrite(SIREN_2, LOW);
  Serial.println("Pins Initialized");

  // Initialize VL53L8CX
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
  // Initialize DS18B20
  sensors.begin();
  Serial.println("DS18B20 Initialized.");

  getDeviceMAC();
  checkPowerStatus();
  connectNetwork();
  Serial.println("--- Setup Complete ---");
}

void loop() {
  checkPowerStatus();
  readEnvironment();
  countCrowd();
  triggerAlertSiren();
  triggerClearSiren();
  uploadData();
}


