#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

// Pin Definitions - THREE SEPARATE BUSES
#define LEFT_SENSORS_BUS 4     // DS18B20 data line for left side sensors (2-3 sensors)
#define RIGHT_SENSORS_BUS 15   // DS18B20 data line for right side sensors (2-3 sensors)
#define HEATER_SENSOR_BUS 16   // DS18B20 data line for heater sensor (1 sensor - CRITICAL)

#define HEATER_SSR_PIN 25      // GPIO for 3V SSR (heater control)
#define FAN_RELAY_PIN 26       // GPIO for 5V relay (AC fans control) via transistor
#define COOLING_FAN_PIN 27     // GPIO for 5V DC cooling fan via transistor

// TFT Display Pins (Hardware SPI)
#define TFT_CS    5
#define TFT_RST   17
#define TFT_DC    2            // Changed from 16 (now used for heater sensor)
#define TFT_SCLK  18           // SCK
#define TFT_MOSI  23           // SDA/MOSI

// Temperature Thresholds
#define TEMP_MIN 10.0          // Turn on heating below this
#define TEMP_TARGET 12.0       // Target temperature when heating
#define TEMP_MAX 25.0          // Turn on cooling above this
#define HEATER_SAFETY_MAX 35.0 // Safety cutoff for heater sensor
#define HYSTERESIS 0.5         // Prevent rapid on/off cycling

// Timing
#define AIR_TEMP_READ_INTERVAL 5000     // Read air temps every 5 seconds
#define HEATER_TEMP_READ_INTERVAL 1000  // Read heater temp every 1 second (SAFETY)
#define DISPLAY_UPDATE_INTERVAL 2000    // Update display every 2 seconds

// Setup OneWire buses (THREE SEPARATE BUSES)
OneWire leftSensorsBus(LEFT_SENSORS_BUS);
OneWire rightSensorsBus(RIGHT_SENSORS_BUS);
OneWire heaterSensorBus(HEATER_SENSOR_BUS);

// Dallas Temperature objects (THREE SEPARATE INSTANCES)
DallasTemperature leftSensors(&leftSensorsBus);
DallasTemperature rightSensors(&rightSensorsBus);
DallasTemperature heaterSensor(&heaterSensorBus);

// TFT Display
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

// Sensor addresses and counts
DeviceAddress leftSensorAddresses[3];
DeviceAddress rightSensorAddresses[3];
DeviceAddress heaterSensorAddress;

int numLeftSensors = 0;
int numRightSensors = 0;
bool heaterSensorDetected = false;

// Temperature readings
float leftTemperatures[3];
float rightTemperatures[3];
float averageTemp = 0.0;
float heaterTemp = 0.0;

// System state
bool heaterOn = false;
bool fansOn = false;
bool coolingFanOn = true;
bool heaterSensorFault = false;
bool airSensorFault = false;

// Timing variables
unsigned long lastAirTempRead = 0;
unsigned long lastHeaterTempRead = 0;
unsigned long lastDisplayUpdate = 0;

// Consecutive error counting
int heaterSensorErrorCount = 0;
int leftSensorErrorCount = 0;
int rightSensorErrorCount = 0;
#define MAX_SENSOR_ERRORS 3

// Function declarations
void readHeaterTemperature();
void readLeftTemperatures();
void readRightTemperatures();
void calculateAverage();
void controlSystem();
void heaterSafetyCheck();
void emergencyShutdown();
void updateDisplay();
void printAddress(DeviceAddress deviceAddress);

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n=================================");
  Serial.println("GREENHOUSE CONTROLLER v3.0");
  Serial.println("THREE BUS ARCHITECTURE");
  Serial.println("=================================");
  Serial.println("Bus 1 (GPIO 4):  LEFT sensors");
  Serial.println("Bus 2 (GPIO 15): RIGHT sensors");
  Serial.println("Bus 3 (GPIO 16): HEATER sensor");
  Serial.println("=================================\n");
  
  // Initialize pins
  pinMode(HEATER_SSR_PIN, OUTPUT);
  pinMode(FAN_RELAY_PIN, OUTPUT);
  pinMode(COOLING_FAN_PIN, OUTPUT);
  
  // Ensure everything starts OFF (SAFETY)
  digitalWrite(HEATER_SSR_PIN, LOW);
  digitalWrite(FAN_RELAY_PIN, LOW);
  digitalWrite(COOLING_FAN_PIN, HIGH); // Turn on cooling fan immediately
  
  Serial.println("All outputs initialized to SAFE state");
  Serial.println();
  
  // Initialize LEFT sensors (Bus 1)
  Serial.println("Initializing LEFT sensor bus (GPIO 4)...");
  leftSensors.begin();
  numLeftSensors = leftSensors.getDeviceCount();
  Serial.print("  Found ");
  Serial.print(numLeftSensors);
  Serial.println(" sensor(s)");
  
  for (int i = 0; i < numLeftSensors && i < 3; i++) {
    if (leftSensors.getAddress(leftSensorAddresses[i], i)) {
      Serial.print("  L");
      Serial.print(i);
      Serial.print(" Address: ");
      printAddress(leftSensorAddresses[i]);
      Serial.println();
      leftSensors.setResolution(leftSensorAddresses[i], 12);
    }
  }
  Serial.println();
  
  // Initialize RIGHT sensors (Bus 2)
  Serial.println("Initializing RIGHT sensor bus (GPIO 15)...");
  rightSensors.begin();
  numRightSensors = rightSensors.getDeviceCount();
  Serial.print("  Found ");
  Serial.print(numRightSensors);
  Serial.println(" sensor(s)");
  
  for (int i = 0; i < numRightSensors && i < 3; i++) {
    if (rightSensors.getAddress(rightSensorAddresses[i], i)) {
      Serial.print("  R");
      Serial.print(i);
      Serial.print(" Address: ");
      printAddress(rightSensorAddresses[i]);
      Serial.println();
      rightSensors.setResolution(rightSensorAddresses[i], 12);
    }
  }
  Serial.println();
  
  // Initialize HEATER sensor (Bus 3 - CRITICAL)
  Serial.println("Initializing HEATER sensor bus (GPIO 16)...");
  heaterSensor.begin();
  int heaterSensorCount = heaterSensor.getDeviceCount();
  Serial.print("  Found ");
  Serial.print(heaterSensorCount);
  Serial.println(" sensor(s)");
  
  if (heaterSensorCount == 1) {
    if (heaterSensor.getAddress(heaterSensorAddress, 0)) {
      Serial.print("  Heater Sensor Address: ");
      printAddress(heaterSensorAddress);
      Serial.println();
      heaterSensor.setResolution(heaterSensorAddress, 12);
      heaterSensorDetected = true;
      Serial.println("  ✓ Heater sensor OK");
    }
  } else {
    Serial.println("  ✗ ERROR: Expected exactly 1 heater sensor!");
    Serial.println("  HEATER OPERATION DISABLED!");
    heaterSensorDetected = false;
  }
  Serial.println();
  
  // Validate sensor counts
  int totalAirSensors = numLeftSensors + numRightSensors;
  Serial.print("Total air sensors: ");
  Serial.println(totalAirSensors);
  
  if (totalAirSensors < 4) {
    Serial.println("WARNING: Expected at least 4 air sensors!");
    Serial.println("System will continue with reduced sensor coverage.");
  }
  
  // Initialize TFT display
  Serial.println("\nInitializing display...");
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1); // Landscape mode
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(0, 0);
  tft.println("Greenhouse Control");
  tft.println("3-Bus Architecture");
  tft.print("L:");
  tft.print(numLeftSensors);
  tft.print(" R:");
  tft.print(numRightSensors);
  tft.print(" H:");
  tft.println(heaterSensorDetected ? "1" : "0");
  tft.println("Starting...");
  
  delay(2000);
  
  Serial.println("=================================");
  Serial.println("Setup complete - entering main loop");
  Serial.println("=================================\n");
}

void loop() {
  unsigned long currentMillis = millis();
  
  // Read HEATER temperature (FREQUENT - SAFETY CRITICAL)
  if (currentMillis - lastHeaterTempRead >= HEATER_TEMP_READ_INTERVAL) {
    lastHeaterTempRead = currentMillis;
    readHeaterTemperature();
    heaterSafetyCheck();
  }
  
  // Read AIR temperatures from BOTH buses (LESS FREQUENT)
  if (currentMillis - lastAirTempRead >= AIR_TEMP_READ_INTERVAL) {
    lastAirTempRead = currentMillis;
    readLeftTemperatures();
    readRightTemperatures();
    calculateAverage();
    controlSystem();
  }
  
  // Update display
  if (currentMillis - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
    lastDisplayUpdate = currentMillis;
    updateDisplay();
  }
}

void readHeaterTemperature() {
  if (!heaterSensorDetected) {
    heaterTemp = -127.0;
    heaterSensorFault = true;
    return;
  }
  
  heaterSensor.requestTemperatures();
  float temp = heaterSensor.getTempC(heaterSensorAddress);
  
  if (temp != DEVICE_DISCONNECTED_C && temp > -50 && temp < 100) {
    heaterTemp = temp;
    heaterSensorErrorCount = 0;
    heaterSensorFault = false;
  } else {
    heaterSensorErrorCount++;
    if (heaterSensorErrorCount >= MAX_SENSOR_ERRORS) {
      Serial.println("ERROR: Heater sensor fault!");
      heaterTemp = -127.0;
      heaterSensorFault = true;
    }
  }
}

void readLeftTemperatures() {
  leftSensors.requestTemperatures();
  
  int validCount = 0;
  for (int i = 0; i < numLeftSensors && i < 3; i++) {
    float temp = leftSensors.getTempC(leftSensorAddresses[i]);
    if (temp != DEVICE_DISCONNECTED_C && temp > -50 && temp < 60) {
      leftTemperatures[i] = temp;
      validCount++;
    } else {
      leftTemperatures[i] = -127.0;
    }
  }
  
  if (validCount < numLeftSensors / 2) {
    leftSensorErrorCount++;
  } else {
    leftSensorErrorCount = 0;
  }
}

void readRightTemperatures() {
  rightSensors.requestTemperatures();
  
  int validCount = 0;
  for (int i = 0; i < numRightSensors && i < 3; i++) {
    float temp = rightSensors.getTempC(rightSensorAddresses[i]);
    if (temp != DEVICE_DISCONNECTED_C && temp > -50 && temp < 60) {
      rightTemperatures[i] = temp;
      validCount++;
    } else {
      rightTemperatures[i] = -127.0;
    }
  }
  
  if (validCount < numRightSensors / 2) {
    rightSensorErrorCount++;
  } else {
    rightSensorErrorCount = 0;
  }
}

void calculateAverage() {
  float sum = 0.0;
  int validCount = 0;
  
  // Add LEFT sensors
  for (int i = 0; i < numLeftSensors && i < 3; i++) {
    if (leftTemperatures[i] > -100) {
      sum += leftTemperatures[i];
      validCount++;
    }
  }
  
  // Add RIGHT sensors
  for (int i = 0; i < numRightSensors && i < 3; i++) {
    if (rightTemperatures[i] > -100) {
      sum += rightTemperatures[i];
      validCount++;
    }
  }
  
  if (validCount >= 3) { // Need at least 3 working sensors
    averageTemp = sum / validCount;
    airSensorFault = false;
  } else {
    averageTemp = -127.0;
    if (leftSensorErrorCount >= MAX_SENSOR_ERRORS || rightSensorErrorCount >= MAX_SENSOR_ERRORS) {
      airSensorFault = true;
      Serial.print("WARNING: Insufficient air sensors (");
      Serial.print(validCount);
      Serial.println(" working)");
    }
  }
}

void controlSystem() {
  // DO NOT operate if sensors are faulty
  if (heaterSensorFault || airSensorFault) {
    if (heaterOn || fansOn) {
      Serial.println("FAULT: Shutting down due to sensor failure");
      emergencyShutdown();
    }
    return;
  }
  
  // HEATING MODE: Average temp below minimum
  if (averageTemp < TEMP_MIN) {
    if (!heaterOn && heaterTemp < HEATER_SAFETY_MAX - 2.0) {
      heaterOn = true;
      fansOn = true;
      digitalWrite(HEATER_SSR_PIN, HIGH);
      digitalWrite(FAN_RELAY_PIN, HIGH);
      Serial.print("HEATING MODE: ON | Avg: ");
      Serial.print(averageTemp, 1);
      Serial.print("°C | Heater: ");
      Serial.print(heaterTemp, 1);
      Serial.println("°C");
    }
    
    // Continue heating until target reached
    if (averageTemp >= TEMP_TARGET + HYSTERESIS && heaterOn) {
      heaterOn = false;
      fansOn = false;
      digitalWrite(HEATER_SSR_PIN, LOW);
      digitalWrite(FAN_RELAY_PIN, LOW);
      Serial.println("Target reached: Heater and fans OFF");
    }
  }
  
  // COOLING MODE: Average temp above maximum
  else if (averageTemp > TEMP_MAX) {
    if (!fansOn) {
      if (heaterOn) {
        heaterOn = false;
        digitalWrite(HEATER_SSR_PIN, LOW);
      }
      fansOn = true;
      digitalWrite(FAN_RELAY_PIN, HIGH);
      Serial.print("COOLING MODE: Fans ON | Avg: ");
      Serial.print(averageTemp, 1);
      Serial.println("°C");
    }
    
    // Turn off fans when temp drops
    if (averageTemp <= TEMP_MAX - HYSTERESIS && fansOn && !heaterOn) {
      fansOn = false;
      digitalWrite(FAN_RELAY_PIN, LOW);
      Serial.println("Cooling complete: Fans OFF");
    }
  }
  
  // IDLE MODE: Temperature in acceptable range
  else {
    if (heaterOn || (fansOn && !heaterOn)) {
      heaterOn = false;
      fansOn = false;
      digitalWrite(HEATER_SSR_PIN, LOW);
      digitalWrite(FAN_RELAY_PIN, LOW);
      Serial.print("IDLE MODE | Avg: ");
      Serial.print(averageTemp, 1);
      Serial.println("°C");
    }
  }
}

void heaterSafetyCheck() {
  // CRITICAL: Heater sensor failure
  if (heaterSensorFault && heaterOn) {
    Serial.println("CRITICAL SAFETY: Heater sensor failed!");
    emergencyShutdown();
    return;
  }
  
  // CRITICAL: Never allow heater on without fans
  if (heaterOn && !fansOn) {
    Serial.println("CRITICAL SAFETY: Heater on without fans!");
    emergencyShutdown();
    return;
  }
  
  // CRITICAL: Heater sensor over temperature
  if (heaterTemp >= HEATER_SAFETY_MAX) {
    Serial.print("CRITICAL SAFETY: Heater temp too high: ");
    Serial.print(heaterTemp, 1);
    Serial.println("°C");
    
    if (heaterOn) {
      heaterOn = false;
      digitalWrite(HEATER_SSR_PIN, LOW);
      Serial.println("Heater DISABLED");
    }
    
    // Keep fans running to cool down
    if (!fansOn) {
      fansOn = true;
      digitalWrite(FAN_RELAY_PIN, HIGH);
      Serial.println("Emergency cooling: Fans ON");
    }
  }
}

void emergencyShutdown() {
  heaterOn = false;
  fansOn = false;
  digitalWrite(HEATER_SSR_PIN, LOW);
  digitalWrite(FAN_RELAY_PIN, LOW);
  Serial.println("=== EMERGENCY SHUTDOWN ===");
}

void updateDisplay() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(0, 0);
  tft.setTextSize(1);
  
  // Title
  tft.setTextColor(ST77XX_GREEN);
  tft.println("GREENHOUSE 3-BUS");
  tft.println("================");
  
  // Fault indicators
  if (heaterSensorFault || airSensorFault) {
    tft.setTextColor(ST77XX_RED);
    tft.setTextSize(2);
    tft.println("FAULT!");
    tft.setTextSize(1);
    if (heaterSensorFault) tft.println("Heater sensor");
    if (airSensorFault) tft.println("Air sensors");
    tft.println();
  }
  
  // Average temperature
  tft.setTextSize(2);
  if (averageTemp > -100) {
    tft.setTextColor(ST77XX_YELLOW);
    tft.print("Avg:");
    tft.print(averageTemp, 1);
    tft.println("C");
  } else {
    tft.setTextColor(ST77XX_RED);
    tft.println("Avg: ERR");
  }
  
  // Heater sensor temperature
  tft.setTextSize(1);
  if (heaterTemp > -100) {
    if (heaterTemp >= HEATER_SAFETY_MAX - 3) {
      tft.setTextColor(ST77XX_RED);
    } else if (heaterTemp >= HEATER_SAFETY_MAX - 5) {
      tft.setTextColor(ST77XX_ORANGE);
    } else {
      tft.setTextColor(ST77XX_CYAN);
    }
    tft.print("Heat:");
    tft.print(heaterTemp, 1);
    tft.println("C");
  } else {
    tft.setTextColor(ST77XX_RED);
    tft.println("Heat: FAULT");
  }
  
  // System status
  tft.setTextColor(ST77XX_WHITE);
  tft.println();
  tft.print("HTR:");
  tft.setTextColor(heaterOn ? ST77XX_RED : ST77XX_WHITE);
  tft.print(heaterOn ? "ON " : "OFF");
  
  tft.setTextColor(ST77XX_WHITE);
  tft.print(" FAN:");
  tft.setTextColor(fansOn ? ST77XX_GREEN : ST77XX_WHITE);
  tft.println(fansOn ? "ON" : "OFF");
  
  // LEFT sensors
  tft.setTextColor(ST77XX_BLUE);
  tft.println();
  tft.println("LEFT:");
  for (int i = 0; i < numLeftSensors && i < 3; i++) {
    tft.print("L");
    tft.print(i);
    tft.print(":");
    if (leftTemperatures[i] > -100) {
      tft.print(leftTemperatures[i], 1);
    } else {
      tft.setTextColor(ST77XX_RED);
      tft.print("ERR");
      tft.setTextColor(ST77XX_BLUE);
    }
    tft.print(" ");
  }
  
  // RIGHT sensors
  tft.println();
  tft.println("RIGHT:");
  for (int i = 0; i < numRightSensors && i < 3; i++) {
    tft.print("R");
    tft.print(i);
    tft.print(":");
    if (rightTemperatures[i] > -100) {
      tft.print(rightTemperatures[i], 1);
    } else {
      tft.setTextColor(ST77XX_RED);
      tft.print("ERR");
      tft.setTextColor(ST77XX_BLUE);
    }
    tft.print(" ");
  }
}

void printAddress(DeviceAddress deviceAddress) {
  for (uint8_t i = 0; i < 8; i++) {
    if (deviceAddress[i] < 16) Serial.print("0");
    Serial.print(deviceAddress[i], HEX);
  }
}