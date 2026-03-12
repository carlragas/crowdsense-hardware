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
#define GAS_DIGITAL 33
#define GAS_ANALOG 35

// Object Initialization
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
VL53L1X distanceSensor;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

bool tofSuccess = false;

void setup() {
  Serial.begin(115200);
  delay(1000); // Give serial time to stabilize
  Serial.println("\n--- System Booting ---");

  // 1. Initialize I2C
  Wire.begin(I2C_SDA, I2C_SCL);

  // 2. Initialize OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED Failed!");
  } else {
    display.clearDisplay();
    display.setTextColor(WHITE);
    display.setCursor(0,0);
    display.println("Booting...");
    display.display();
  }

  // 3. Initialize VL53L1X (The likely crash point)
  distanceSensor.setTimeout(500);
  if (!distanceSensor.init()) {
    Serial.println("CRITICAL: VL53L1X sensor not found!");
    tofSuccess = false;
  } else {
    distanceSensor.setDistanceMode(VL53L1X::Long);
    distanceSensor.setMeasurementTimingBudget(50000);
    distanceSensor.startContinuous(50);
    tofSuccess = true;
    Serial.println("VL53L1X Initialized.");
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
  // Read Flame & Gas (Analog/Digital)
  int flameValue = analogRead(FLAME_ANALOG);
  bool flameDetected = !digitalRead(FLAME_DIGITAL);
  int gasValue = analogRead(GAS_ANALOG);
  bool gasAlert = digitalRead(GAS_DIGITAL);

  // Read ToF ONLY if it initialized correctly
  uint16_t distance = 0;
  if (tofSuccess) {
    distance = distanceSensor.read();
  }

  // Read Temperature
  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);

  // --- Output to Serial ---
  Serial.print("Dist: "); Serial.print(distance); Serial.print("mm | ");
  Serial.print("Temp: "); Serial.print(tempC); Serial.print("C | ");
  Serial.print("Gas: "); Serial.print(gasValue); Serial.print(" | ");
  Serial.print("Flame: "); Serial.println(flameValue);

  // --- Update OLED ---
  display.clearDisplay();
  display.setCursor(0,0);
  display.print("Dist: "); display.print(distance); display.println(" mm");
  display.print("Temp: "); display.print(tempC); display.println(" C");
  display.print("Gas:  "); display.println(gasValue);
  display.print("Flame:"); display.println(flameValue);
  display.display();

  delay(1000);
}