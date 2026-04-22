// Libraries
#include <Wire.h>
#include <vl53l7cx_class.h>
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
#define SIREN_2 25 //Alert Siren
#define SIREN_1 26 //Clear Siren
#define I2C_SDA 21
#define I2C_SCL 22
#define UPS_POWER_INDICATOR 32
#define GAS_DIGITAL 33
#define BACKUP_FLAME_ANALOG 34
#define GAS 35
#define FIREBASE_HOST "https://crowdsense-db-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define FIREBASE_LEGACY_TOKEN "5mGeiwSA9PLndbFmJZtC8x7a9U78VaM0H21nh1nd"

//Initializations
VL53L7CX sensor(&Wire, -1, -1); // The library expects (Wire, LPN_PIN, RST_PIN). Using -1 for unused reset pins.
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

//Database Variables
const unsigned long FIREBASE_SEND_INTERVAL = 900000; // Interval for sending data in the database
unsigned long lastFirebaseSendTime = 0;
bool firebaseConnected = false;
String deviceMAC = "00:00:00:00:00:00";
String pathBase;
//ToF Variables
bool tofSuccess = false;
const int PERSON_THRESHOLD_MM = 500; 
int totalInside = 0;
int totalEntries = 0;
int totalExits = 0;
// ToF Variable: MULTI-LANE TRACKING: 4 separate state machines for columns 0, 1, 2, and 3
int laneState[4] = {0, 0, 0, 0}; 
// ToF Variables: Cooldown timers to prevent a single person triggering multiple lanes at once
unsigned long lastEntryTime = 0;
unsigned long lastExitTime = 0;
const int EVENT_COOLDOWN_MS = 800; 
// Environment Variables
float currentTempC = 0.0;
int currentGasValue = 0;
bool currentMainFlameValue = true;
int currentBackupFlameValue = 0;
float tempThreshold;
int flameThreshold;
int gasThreshold;
bool esp32Online = true;
unsigned long lastEnvReadTime = 0; 
// Siren Variables
bool emergencyMode = false;
bool sirenAlertActive = false;
bool sirenClearActive = false;
unsigned long sirenAlertDuration = 0;
unsigned long sirenClearDuration = 0;
String pathAlertOn, pathAlertOff;
const unsigned long ManualCheckInterval = 60000;
unsigned long lastManualCheckTime = 0;
// Power Variables - Voltage Divider
const unsigned long checkPowerInterval = 5000;
unsigned long lastPowerCheckedTime = 0;
const float Resistor1 = 10000.0;
const float Resistor2 = 3300.0;
const float powerRatio = (Resistor1 + Resistor2)/Resistor2;
const float upperPowerThreshold = 11.5;
const float lowerPowerThreshold = 10.8;
String powerStatus;

void pinConfig(){
  pinMode(BACKUP_FLAME_DIGITAL, INPUT);
  pinMode(MAIN_FLAME, INPUT_PULLUP);
  pinMode(GAS_DIGITAL, INPUT);
  pinMode(UPS_POWER_INDICATOR, INPUT);
  pinMode(SIREN_1, OUTPUT);
  pinMode(SIREN_2, OUTPUT);
}

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
  bool res = wm.autoConnect("CrowdSense_Main_Ext", "12345678");
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
    }
  }
}

void getSensorThreshold(){
  if (Firebase.ready()){
    if (Firebase.RTDB.getFloat(&fbdo, (pathBase + "temperature_threshold").c_str())){
       if (fbdo.dataType() == "float"){
        tempThreshold = fbdo.floatData();
       } else if (fbdo.dataType() == "null"){
        Serial.println("Temperature threshold path not found. Assigning default threshold values.");
        tempThreshold = 57.0;
       }
    }

    if (Firebase.RTDB.getInt(&fbdo, (pathBase + "smoke_threshold").c_str())){
       if (fbdo.dataType() == "int"){
        gasThreshold = fbdo.intData();
       } else if (fbdo.dataType() == "null"){
        Serial.println("Gas threshold path not found. Assigning default threshold values.");
        gasThreshold = 500;
       }
    }

    if (Firebase.RTDB.getInt(&fbdo, (pathBase + "flame_threshold").c_str())){
       if (fbdo.dataType() == "int"){
        flameThreshold = fbdo.intData();
       } else if (fbdo.dataType() == "null"){
        Serial.println("Flame threshold path not found. Assigning default threshold values.");
        flameThreshold = 2000;
       }
    }
  
  } else {
    Serial.println("Unable to connect to database. Assigning default threshold values.");
    tempThreshold = 57.0;
    flameThreshold = 2000;
    gasThreshold = 500;
  }
}

bool manualTrigger(int caller){
  if (Firebase.ready()){
    String manualAlertOnPath = "/sensor_data" + deviceMAC + "/manual_alert_on";
    String manualAlertOffPath = "/sensor_data" + deviceMAC + "/manual_alert_off";
    String manualClearOnPath = "/sensor_data" + deviceMAC + "/manual_clear_on";
    String manualClearOffPath = "/sensor_data" + deviceMAC + "/manual_clear_off";
    switch(caller){
      case 1:
        if (!sirenAlertActive){
          if (Firebase.RTDB.getBool(&fbdo, manualAlertOnPath.c_str())) {
            return fbdo.boolData();
          }
        } else if (sirenAlertActive){
          if (Firebase.RTDB.getBool(&fbdo, manualAlertOffPath.c_str())){
            return fbdo.boolData();
          }
        }
        if ((!Firebase.RTDB.getBool(&fbdo, manualAlertOnPath.c_str())) || (Firebase.RTDB.getBool(&fbdo, manualAlertOffPath.c_str()))){
          return false;
          Serial.println("ERROR: Unable to retrieve manual alert status.");
        }
        break;

      case 2:
        if (!sirenClearActive){
          
          if (Firebase.RTDB.getBool(&fbdo, manualClearOnPath.c_str())) {
            return fbdo.boolData();
          }
        }
        else if (sirenClearActive){
          
          if (Firebase.RTDB.getBool(&fbdo, manualClearOffPath.c_str())) {
            return fbdo.boolData();
          }
        }
        if ((!Firebase.RTDB.getBool(&fbdo, manualClearOnPath.c_str())) || (Firebase.RTDB.getBool(&fbdo, manualClearOffPath.c_str()))){
          return false;
          Serial.println("ERROR: Unable to retrieve manual alert status.");
        }
        break;

      default:
        return false;
        Serial.println("ERROR: Unknown Caller.");
    }
  }
}

void activateAlertSiren(){
  if (!sirenAlertActive && !emergencyMode){
    bool isFireDetected = (!currentMainFlameValue || currentBackupFlameValue <= flameThreshold) && (currentGasValue >= gasThreshold);
    bool manualAlert = false;
    if (firebaseConnected && (millis() - lastManualCheckTime >= ManualCheckInterval)){
      lastManualCheckTime = millis();
      manualAlert = manualTrigger(1);
    }
    if (isFireDetected){
      sirenAlertActive =  true;
      emergencyMode = true;
      digitalWrite(SIREN_2,HIGH);
      sirenAlertDuration = millis() + 180000;
      Serial.println("FIRE DETECTED: Emergency mode enabled. Siren Activated.");
    }
    else if (manualAlert){
      manualAlert = false;
      sirenAlertActive =  true;
      emergencyMode = true;
      digitalWrite(SIREN_2,HIGH);
      sirenAlertDuration = millis() + 180000;
      Serial.println("MANUAL OVERRIDE: Emergency mode enabled. Siren Activated.");
    }
  }
}

void deactivateAlertSiren(){
  if (sirenAlertActive &&  emergencyMode){
    bool manualAlert = false;
    if (firebaseConnected && (millis() - lastManualCheckTime >= ManualCheckInterval)){
      lastManualCheckTime = millis();
      manualAlert = manualTrigger(1);
    }
    if (millis() >= sirenAlertDuration){
      sirenAlertDuration = 0;
      sirenAlertActive = false;
      digitalWrite(SIREN_2, LOW);
      Serial.println("SIREN TIMEOUT! Alert siren deactivated. Emergency mode still enabled.");
    }
    else if (manualAlert) {
      sirenAlertDuration = 0;
      sirenAlertActive = false;
      emergencyMode = false;
      digitalWrite(SIREN_2, LOW);
      Serial.println("MANUAL OVERRIDE! Alert siren deactivated. Emergency mode disabled.");
    }
  }
}

void activateClearSiren(){
  if (!sirenClearActive && emergencyMode){
    bool clearStatus = totalInside == 0; 
    bool manualClear = false;
    if (firebaseConnected && (millis() - lastManualCheckTime >= ManualCheckInterval)){
      lastManualCheckTime = millis();
      manualClear = manualTrigger(2);
    }

    if (clearStatus){
      sirenClearActive = true;
      digitalWrite(SIREN_1, HIGH);
      sirenClearDuration = millis() + 60000;
      emergencyMode = false;
      Serial.println("AREA CLEAR: All personnel had successfully evacuated the area.");
    }

    if (manualClear){
        sirenClearActive = true;
        digitalWrite(SIREN_1, HIGH);
        sirenClearDuration = millis() + 60000;
        emergencyMode = false;
        Serial.println("MANUAL OVERRIDE: All personnel had successfully evacuated the area.");
      }
  }
}

void deactivateClearSiren(){
  if (sirenClearActive){
    bool manualClear = false;
    if (firebaseConnected && (millis() - lastManualCheckTime >= ManualCheckInterval)){
      lastManualCheckTime = millis();
      manualClear = manualTrigger(2);
    }

    if (millis() >= sirenClearDuration) {
      sirenClearActive = false;
      digitalWrite(SIREN_1, LOW);
      Serial.println("SIREN TIMEOUT: Clear siren deactivated.");
    }

    if(manualClear){
      sirenClearActive = false;
      digitalWrite(SIREN_1, LOW);
      Serial.println("MANUAL OVERRIDE: Clear siren deactivated.");
    }
  }
}

void uploadData(){
  if (firebaseConnected && (millis() - lastFirebaseSendTime >= FIREBASE_SEND_INTERVAL)) {
    lastFirebaseSendTime = millis();
    
    // CRITICAL: Update the NTP client to get the latest time from the internet
    timeClient.update();
    unsigned long epochTime = timeClient.getEpochTime();

    // Only upload if we have a valid year (Epoch > 1,000,000 means we are past 1970)
    if (Firebase.ready() && epochTime > 1000000) {
      // FIX: Use double. It can safely hold the massive 13-digit millisecond number.
      double currentEpochMillis = (double)epochTime * 1000.0;

      // Send Data
      Firebase.RTDB.setFloat(&fbdo, (pathBase + "temperature").c_str(), currentTempC);
      Firebase.RTDB.setInt(&fbdo, (pathBase + "gas").c_str(), currentGasValue);
      Firebase.RTDB.setInt(&fbdo, (pathBase + "flame").c_str(), currentBackupFlameValue);
      Firebase.RTDB.setInt(&fbdo, (pathBase + "people_inside").c_str(), totalInside);
      Firebase.RTDB.setInt(&fbdo, (pathBase + "total_entries").c_str(), totalEntries);
      Firebase.RTDB.setInt(&fbdo, (pathBase + "total_exits").c_str(), totalExits);
      Firebase.RTDB.setBool(&fbdo, (pathBase + "siren_alert_active").c_str(), sirenAlertActive);
      Firebase.RTDB.setBool(&fbdo, (pathBase + "siren_clear_active").c_str(), sirenClearActive);
      Firebase.RTDB.setString(&fbdo, (pathBase + "power_status").c_str(), powerStatus);
      
      // FIX: Use setDouble instead of setLongLong
      Firebase.RTDB.setDouble(&fbdo, (pathBase + "last_updated").c_str(), currentEpochMillis);
  
      Serial.println("--- Firebase data update complete (Synced with NTP) ---");
      
    } else if (epochTime <= 1000000) {
      Serial.println("Waiting for NTP sync... skipping Firebase upload to avoid 1970 error.");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000); 
  Serial.println("\n--- System Booting ---");
  // Initialize I2C for ESP32
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000); 
  pinConfig();
  digitalWrite(SIREN_1, HIGH);
  digitalWrite(SIREN_2, HIGH);
  // Initialize VL53L7CX 
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
  // Initialize DS18B20
  sensors.begin();
  Serial.println("DS18B20 Initialized.");

  getDeviceMAC();

  pathBase = "/sensor_data/" + deviceMAC + "/";
  getSensorThreshold();

  checkPowerStatus();
  connectNetwork();
  Serial.println("--- Setup Complete ---");
}

void loop() {
  checkPowerStatus();
  readEnvironment();
  countCrowd();
  activateAlertSiren();
  deactivateAlertSiren();
  activateClearSiren();
  deactivateClearSiren();
  uploadData();
}