i#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <VL53L1X.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ============ CONFIGURATION ============
// I2C
#define I2C_SDA 21
#define I2C_SCL 22

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// VL53L1X ToF Sensor
VL53L1X tofSensor;

// DS18B20 Temperature
#define ONE_WIRE_BUS 4
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);
DeviceAddress tempAddress;

// KY-026 Flame Sensor
#define FLAME_DIGITAL 5
#define FLAME_ANALOG 34
int flameBaseline = 0;

// MQ-2 Gas Sensor
#define GAS_DIGITAL 32
#define GAS_ANALOG 35
int gasBaseline = 0;

// Timing
unsigned long lastReading = 0;
const unsigned long readingInterval = 500; // Read every 500ms

// Display mode
int displayMode = 0;
unsigned long lastModeChange = 0;
const long modeInterval = 5000; // Change display every 5 seconds

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n=== 4-IN-1 SENSOR SYSTEM ===");
  
  // Initialize I2C
  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.println("I2C initialized");
  
  // Initialize OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found!");
  } else {
    Serial.println("OLED initialized");
  }
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Initializing...");
  display.display();
  
  // Initialize sensors
  initToFSensor();
  initTempSensor();
  initFlameSensor();
  initGasSensor();
  
  // Show ready
  display.clearDisplay();
  display.println("All Sensors OK!");
  display.println("4-in-1 Monitor");
  display.display();
  delay(2000);
}

// ============ SENSOR INITIALIZATION ============
void initToFSensor() {
  Serial.print("VL53L1X: ");
  if (tofSensor.init()) {
    tofSensor.setDistanceMode(VL53L1X::Long);
    tofSensor.setMeasurementTimingBudget(50000);
    tofSensor.startContinuous(50);
    Serial.println("OK");
  } else {
    Serial.println("FAILED");
  }
}

void initTempSensor() {
  Serial.print("DS18B20: ");
  tempSensor.begin();
  if (tempSensor.getDeviceCount() > 0) {
    tempSensor.getAddress(tempAddress, 0);
    tempSensor.setResolution(tempAddress, 12);
    Serial.println("OK");
  } else {
    Serial.println("FAILED");
  }
}

void initFlameSensor() {
  Serial.print("KY-026: ");
  pinMode(FLAME_DIGITAL, INPUT);
  pinMode(FLAME_ANALOG, INPUT);
  
  // Calibrate
  long sum = 0;
  for(int i = 0; i < 50; i++) {
    sum += analogRead(FLAME_ANALOG);
    delay(10);
  }
  flameBaseline = sum / 50;
  Serial.printf("Baseline=%d OK\n", flameBaseline);
}

void initGasSensor() {
  Serial.print("MQ-2: ");
  pinMode(GAS_DIGITAL, INPUT);
  pinMode(GAS_ANALOG, INPUT);
  
  // Quick baseline (should do longer warmup in real use)
  long sum = 0;
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
  int flameIntensity = readFlame();
  int gasLevel = readGas();
  bool flameDetected = digitalRead(FLAME_DIGITAL) == LOW;
  bool gasAlarm = digitalRead(GAS_DIGITAL) == LOW;
  
  // Print to Serial
  if (millis() - lastReading >= 1000) {
    Serial.println("\n--- SENSOR READINGS ---");
    Serial.printf("ToF: %d mm\n", distance);
    Serial.printf("Temp: %.1f °C\n", temperature);
    Serial.printf("Flame: %d%% %s\n", flameIntensity, flameDetected ? "DETECTED" : "none");
    Serial.printf("Gas: %d%% %s\n", gasLevel, gasAlarm ? "ALARM" : "normal");
    lastReading = millis();
  }
  
  // Update display
  updateDisplay(distance, temperature, flameIntensity, flameDetected, gasLevel, gasAlarm);
  
  delay(100);
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

int readFlame() {
  int raw = analogRead(FLAME_ANALOG);
  // Map to percentage (lower raw = more flame)
  return constrain(map(raw, 0, flameBaseline, 100, 0), 0, 100);
}

int readGas() {
  int raw = analogRead(GAS_ANALOG);
  // Map to percentage
  return constrain(map(raw, gasBaseline, 4095, 0, 100), 0, 100);
}

// ============ DISPLAY FUNCTIONS ============
void updateDisplay(int distance, float temp, int flame, bool flameDetected, int gas, bool gasAlarm) {
  // Cycle through display modes
  if (millis() - lastModeChange > modeInterval) {
    displayMode = (displayMode + 1) % 4;
    lastModeChange = millis();
  }
  
  display.clearDisplay();
  
  switch(displayMode) {
    case 0:
      drawMainDashboard(distance, temp, flame, flameDetected, gas, gasAlarm);
      break;
    case 1:
      drawDetailedToF(distance);
      break;
    case 2:
      drawDetailedTemp(temp);
      break;
    case 3:
      drawDetailedSafety(flame, flameDetected, gas, gasAlarm);
      break;
  }
  
  // Draw status bar at bottom
  drawStatusBar(flameDetected, gasAlarm);
  
  display.display();
}

void drawMainDashboard(int distance, float temp, int flame, bool flameDetected, int gas, bool gasAlarm) {
  // Title
  display.setCursor(0, 0);
  display.println("4-in-1 Monitor");
  
  // Distance
  display.setCursor(0, 12);
  display.print("D:");
  display.print(distance);
  display.print("mm");
  
  // Temperature
  display.setCursor(70, 12);
  display.print(temp, 1);
  display.print("C");
  
  // Flame bar
  display.setCursor(0, 25);
  display.print("Flame:");
  display.fillRect(40, 25, map(flame, 0, 100, 0, 50), 8, flameDetected ? SSD1306_WHITE : SSD1306_BLACK);
  display.drawRect(40, 25, 50, 8, SSD1306_WHITE);
  
  // Gas bar
  display.setCursor(0, 38);
  display.print("Gas:");
  display.fillRect(40, 38, map(gas, 0, 100, 0, 50), 8, gasAlarm ? SSD1306_WHITE : SSD1306_BLACK);
  display.drawRect(40, 38, 50, 8, SSD1306_WHITE);
  
  // Alerts
  if (flameDetected) {
    display.setCursor(95, 25);
    display.print("FIRE");
  }
  if (gasAlarm) {
    display.setCursor(95, 38);
    display.print("GAS");
  }
}

void drawDetailedToF(int distance) {
  display.setCursor(0, 0);
  display.println("TOF DISTANCE");
  display.setTextSize(3);
  display.setCursor(0, 20);
  display.print(distance);
  display.print("mm");
  
  // Visual bar
  display.setTextSize(1);
  display.setCursor(0, 55);
  display.print("0");
  display.setCursor(120, 55);
  display.print("4m");
  
  int barWidth = map(constrain(distance, 0, 4000), 0, 4000, 0, 128);
  display.fillRect(0, 48, barWidth, 5, SSD1306_WHITE);
}

void drawDetailedTemp(float temp) {
  display.setCursor(0, 0);
  display.println("TEMPERATURE");
  display.setTextSize(3);
  display.setCursor(0, 20);
  display.print(temp, 1);
  display.print("C");
  
  // Thermometer
  display.setTextSize(1);
  display.setCursor(0, 55);
  display.print("-10C");
  display.setCursor(100, 55);
  display.print("50C");
  
  int barHeight = map(constrain(temp, -10, 50), -10, 50, 0, 40);
  display.fillRect(60, 55-barHeight, 8, barHeight, SSD1306_WHITE);
}

void drawDetailedSafety(int flame, bool flameDetected, int gas, bool gasAlarm) {
  display.setCursor(0, 0);
  display.println("SAFETY STATUS");
  
  // Flame section
  display.setCursor(0, 12);
  display.print("FLAME: ");
  if (flameDetected) {
    display.print("DETECTED!");
    display.fillRect(70, 12, 50, 10, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(75, 12);
    display.print("FIRE");
    display.setTextColor(SSD1306_WHITE);
  } else {
    display.print("safe");
    display.print(" (");
    display.print(flame);
    display.print("%)");
  }
  
  // Gas section
  display.setCursor(0, 27);
  display.print("GAS: ");
  if (gasAlarm) {
    display.print("ALARM!");
    display.fillRect(70, 27, 50, 10, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(75, 27);
    display.print("GAS");
    display.setTextColor(SSD1306_WHITE);
  } else {
    display.print("normal");
    display.print(" (");
    display.print(gas);
    display.print("%)");
  }
  
  // Warning icon if any danger
  if (flameDetected || gasAlarm) {
    display.fillCircle(64, 50, 10, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(61, 46);
    display.print("!");
    display.setTextColor(SSD1306_WHITE);
  }
}

void drawStatusBar(bool flameDetected, bool gasAlarm) {
  display.drawHorizontalLine(0, 62, 128, SSD1306_WHITE);
  
  if (flameDetected || gasAlarm) {
    display.setCursor(0, 55);
    display.print("⚠ DANGER!");
  } else {
    display.setCursor(0, 55);
    display.print("✓ All Normal");
  }
  
  // Mode indicator
  display.setCursor(100, 55);
  display.print("[");
  display.print(displayMode + 1);
  display.print("/4]");
}