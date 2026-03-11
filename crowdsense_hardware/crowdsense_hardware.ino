#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <VL53L1X.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>

// Pin Configuration
#define I2C_SDA 21
#define I2C_SCL 22
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

#define ONE_WIRE_BUS 4
#define FLAME_DIGITAL 5
#define FLAME_ANALOG 34
#define GAS_DIGITAL 32
#define GAS_ANALOG 35

// WiFi Access Point Configuration
const char* apSSID = "ESP32_CrowdSense"
const char* apPassword = "12345678";   

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
VL53L1X tofSensor;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);
DeviceAddress tempAddress;

int flameBaseline = 0;
int gasBaseline = 0;
String macAddress = "";

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n=== CROWDSENSE PROTOTYPE ===");
  
  // Get and display MAC address
  getMACAddress();
  
  // Initialize WiFi Access Point
  initWiFiAP();
  
  Wire.begin(I2C_SDA, I2C_SCL);
  
  // Initialize OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found! Check 0x3C or 0x3D");
  }
  
  showMessage("Initializing...", "Sensors & WiFi", "");
  
  initSensors();
  
  showMessage("All Sensors OK!", "WiFi AP Active", macAddress.substring(0, 17));
  delay(3000);
}

// ============ NEW: WiFi Access Point Function ============
void initWiFiAP() {
  Serial.println("\n--- WiFi Access Point Setup ---");
  
  // Set WiFi mode to Access Point
  WiFi.mode(WIFI_AP);
  
  // Configure the AP
  Serial.print("Setting up AP: ");
  Serial.println(apSSID);
  
  // Start AP with the configured SSID and password
  WiFi.softAP(apSSID, apPassword);
  
  // Get and display AP IP address
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);
  
  Serial.print("MAC Address: ");
  Serial.println(macAddress);
  
  Serial.println("WiFi AP Started!");
  Serial.println("Connect to: " + String(apSSID));
  Serial.println("Password: " + String(apPassword));
  Serial.println("------------------------------\n");
}

// ============ NEW: Get MAC Address ============
void getMACAddress() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  
  // Format MAC address as string
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  
  macAddress = String(macStr);
  
  Serial.println("\n=== DEVICE INFORMATION ===");
  Serial.print("MAC Address: ");
  Serial.println(macAddress);
  Serial.print("ESP32 Chip Model: ");
  Serial.println(ESP.getChipModel());
  Serial.print("Flash Size: ");
  Serial.print(ESP.getFlashChipSize() / (1024 * 1024));
  Serial.println(" MB");
  Serial.println("===========================\n");
}

// ============ SENSOR INIT ============
void initSensors() {
  // ToF
  Serial.print("VL53L1X: ");
  if (tofSensor.init()) {
    tofSensor.setDistanceMode(VL53L1X::Long);
    tofSensor.setMeasurementTimingBudget(50000);
    tofSensor.startContinuous(50);
    Serial.println("OK");
  } else {
    Serial.println("FAILED");
  }
  
  // Temperature
  Serial.print("DS18B20: ");
  tempSensor.begin();
  if (tempSensor.getDeviceCount() > 0) {
    tempSensor.getAddress(tempAddress, 0);
    tempSensor.setResolution(tempAddress, 12);
    Serial.println("OK");
  } else {
    Serial.println("FAILED");
  }
  
  // Flame sensor
  Serial.print("KY-026: ");
  pinMode(FLAME_DIGITAL, INPUT);
  pinMode(FLAME_ANALOG, INPUT);
  
  long sum = 0;
  for(int i = 0; i < 50; i++) {
    sum += analogRead(FLAME_ANALOG);
    delay(10);
  }
  flameBaseline = sum / 50;
  Serial.printf("Baseline=%d OK\n", flameBaseline);
  
  // Gas sensor
  Serial.print("MQ-2: ");
  pinMode(GAS_DIGITAL, INPUT);
  pinMode(GAS_ANALOG, INPUT);
  
  sum = 0;
  for(int i = 0; i < 50; i++) {
    sum += analogRead(GAS_ANALOG);
    delay(10);
  }
  gasBaseline = sum / 50;
  Serial.printf("Baseline=%d OK\n", gasBaseline);
}

// ============ MAIN LOOP ============
void loop() {
  // Read all sensors
  int distance = readToF();
  float temperature = readTemp();
  int flameValue = analogRead(FLAME_ANALOG);
  int gasValue = analogRead(GAS_ANALOG);
  
  bool flameDetected = (flameValue < flameBaseline - 100);
  bool gasAlarm = (gasValue > gasBaseline + 200);
  
  int flamePercent = constrain(map(flameValue, 0, flameBaseline, 100, 0), 0, 100);
  int gasPercent = constrain(map(gasValue, gasBaseline, 4095, 0, 100), 0, 100);
  
  // Print to Serial with WiFi info
  printReadings(distance, temperature, flamePercent, flameDetected, gasPercent, gasAlarm);
  
  // Update display
  updateDisplay(distance, temperature, flamePercent, flameDetected, gasPercent, gasAlarm);
  
  delay(200);
}

// ============ ENHANCED SERIAL OUTPUT ============
void printReadings(int distance, float temp, int flame, bool flameDetected, int gas, bool gasAlarm) {
  static unsigned long lastPrint = 0;
  
  if (millis() - lastPrint >= 2000) { // Print every 2 seconds
    Serial.println("\n=== SENSOR READINGS ===");
    Serial.printf("ToF: %d mm\n", distance);
    Serial.printf("Temp: %.1f °C\n", temp);
    Serial.printf("Flame: %d%% %s\n", flame, flameDetected ? "🔥 DETECTED" : "✓ safe");
    Serial.printf("Gas: %d%% %s\n", gas, gasAlarm ? "⚠️ ALARM" : "✓ normal");
    
    // WiFi Status
    Serial.println("\n--- WiFi AP Status ---");
    Serial.print("SSID: ");
    Serial.println(apSSID);
    Serial.print("IP Address: ");
    Serial.println(WiFi.softAPIP());
    Serial.print("MAC Address: ");
    Serial.println(macAddress);
    Serial.print("Connected Stations: ");
    Serial.println(WiFi.softAPgetStationNum());
    Serial.println("----------------------\n");
    
    lastPrint = millis();
  }
}

// ============ SENSOR READING FUNCTIONS ============
int readToF() {
  tofSensor.read();
  return tofSensor.ranging_data.range_mm;
}

float readTemp() {
  tempSensor.requestTemperatures();
  return tempSensor.getTempC(tempAddress);
}

// ============ SIMPLIFIED DISPLAY WITH WiFi INFO ============
void updateDisplay(int distance, float temp, int flame, bool flameDetected, int gas, bool gasAlarm) {
  display.clearDisplay();
  
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  // Line 1: WiFi Symbol and MAC short
  display.setCursor(0, 0);
  display.print("📶 ");
  display.print(macAddress.substring(9, 17)); // Show last 8 chars of MAC
  display.print(" ");
  display.print(WiFi.softAPgetStationNum());
  display.print(" conn");
  
  // Line 2: Distance and Temp
  display.setCursor(0, 12);
  display.print("D:");
  display.print(distance);
  display.print("mm T:");
  display.print(temp, 1);
  display.println("C");
  
  // Line 3: Flame
  display.setCursor(0, 24);
  display.print("Flame:");
  if (flameDetected) {
    display.print("🔥");
  } else {
    display.print("✓");
  }
  display.print(flame);
  display.print("%");
  
  // Line 4: Gas
  display.setCursor(64, 24);
  display.print("Gas:");
  if (gasAlarm) {
    display.print("⚠️");
  } else {
    display.print("✓");
  }
  display.print(gas);
  display.print("%");
  
  // Bar graphs
  // Flame bar
  display.drawRect(0, 36, 60, 8, SSD1306_WHITE);
  display.fillRect(0, 36, map(flame, 0, 100, 0, 60), 8, SSD1306_WHITE);
  
  // Gas bar
  display.drawRect(68, 36, 60, 8, SSD1306_WHITE);
  display.fillRect(68, 36, map(gas, 0, 100, 0, 60), 8, SSD1306_WHITE);
  
  // Status line
  display.drawLine(0, 50, 127, 50, SSD1306_WHITE);
  
  display.setCursor(0, 52);
  if (flameDetected || gasAlarm) {
    display.print("⚠️ DANGER! ");
  } else {
    display.print("✓ All Normal ");
  }
  
  // Show AP name on bottom right
  display.setCursor(70, 52);
  display.print(apSSID);
  
  display.display();
}

void showMessage(String line1, String line2, String line3) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 10);
  display.println(line1);
  display.setCursor(0, 25);
  display.println(line2);
  display.setCursor(0, 40);
  display.println(line3);
  display.display();
}