#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>

// WiFi Credentials (Access Point Mode)
const char* ap_ssid = "Green";
const char* ap_password = "xD8ro6rNdcxbMWy!P78H";

// Pin Definitions - THREE SEPARATE BUSES
#define LEFT_SENSORS_BUS 4     // DS18B20 data line for left side sensors (2-3 sensors)
#define RIGHT_SENSORS_BUS 15   // DS18B20 data line for right side sensors (2-3 sensors)
#define HEATER_SENSOR_BUS 16   // DS18B20 data line for heater sensor (1 sensor - CRITICAL)

#define HEATER_SSR_PIN 25      // GPIO for 3V SSR (heater control)
#define FAN_RELAY_PIN 26       // GPIO for 5V relay (AC fans control) via transistor
// GPIO 27 for 5V DC cooling fan via transistor is not used anymore

// TFT Display Pins (Hardware SPI)
#define TFT_CS    5
#define TFT_RST   17
#define TFT_DC    2
#define TFT_SCLK  18
#define TFT_MOSI  23

// PWM Configuration for cooling fan
#define COOLING_FAN_PWM_CHANNEL 0
#define COOLING_FAN_PWM_FREQ 25000
#define COOLING_FAN_PWM_RESOLUTION 8

// Temperature Thresholds (adjustable via web interface)

// Temperature Thresholds (adjustable via web interface)
float TEMP_MIN = 10.0;          // Turn on heating below this
float TEMP_TARGET = 12.0;       // Target temperature when heating
float TEMP_MAX = 25.0;          // Turn on cooling above this
float HEATER_SAFETY_MAX = 35.0; // Safety cutoff for heater sensor
#define HYSTERESIS 0.5          // Prevent rapid on/off cycling

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

// Web Server
WebServer server(80);
float lastDisplayedAvg = -999.0;
float lastDisplayedHeater = -999.0;
bool lastHeaterState = false;
bool lastFanState = false;
bool lastFaultState = false;
bool displayInitialized = false;

// Sensor addresses and counts
DeviceAddress leftSensorAddresses[3];
DeviceAddress rightSensorAddresses[3];
DeviceAddress heaterSensorAddress;

int numLeftSensors = 0;
int numRightSensors = 0;
bool heaterSensorDetected = false;

// Temperature readings
float leftTemperatures[3] = {-127.0, -127.0, -127.0};
float rightTemperatures[3] = {-127.0, -127.0, -127.0};
float averageTemp = -127.0;
float heaterTemp = -127.0;

// System state
bool heaterOn = false;
bool fansOn = false;
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

// Web server IP
String webServerIP = "192.168.4.1";

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
void setupWebServer();
void handleRoot();
void handleAdjust();
void handleStatusJSON();
String getHTMLPage();

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n=================================");
  Serial.println("GREENHOUSE CONTROLLER v4.1");
  Serial.println("THREE BUS + WEB INTERFACE");
  Serial.println("=================================");
  Serial.println("Bus 1 (GPIO 4):  LEFT sensors");
  Serial.println("Bus 2 (GPIO 15): RIGHT sensors");
  Serial.println("Bus 3 (GPIO 16): HEATER sensor");
  Serial.println("=================================\n");
  
// Initialize pins
  pinMode(HEATER_SSR_PIN, OUTPUT);
  pinMode(FAN_RELAY_PIN, OUTPUT);
  
  // Ensure everything starts OFF (SAFETY)
  digitalWrite(HEATER_SSR_PIN, LOW);
  digitalWrite(FAN_RELAY_PIN, LOW);

  
  Serial.println("All outputs initialized to SAFE state");
  Serial.println("Electronics cooling fan: ON");
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
      Serial.println("  [OK] Heater sensor detected");
    }
  } else {
    Serial.println("  [ERROR] Expected exactly 1 heater sensor!");
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
  tft.println("3-Bus + Web");
  tft.print("L:");
  tft.print(numLeftSensors);
  tft.print(" R:");
  tft.print(numRightSensors);
  tft.print(" H:");
  tft.println(heaterSensorDetected ? "1" : "0");
  tft.println("Starting WiFi...");
  
  // Setup WiFi Access Point
  Serial.println("\nStarting WiFi Access Point...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);
  webServerIP = IP.toString();
  
  tft.println("WiFi: Green");
  tft.print("IP: ");
  tft.println(IP);
  
  // Setup Web Server
  setupWebServer();
  server.begin();
  Serial.println("Web server started");
  
  delay(2000);
  
  Serial.println("=================================");
  Serial.println("Setup complete - entering main loop");
  Serial.println("=================================\n");
}

void loop() {
  unsigned long currentMillis = millis();
  
  // Handle web server requests
  server.handleClient();
  
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
  
  // Small delay to prevent watchdog issues
  delay(10);
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
  
  if (validCount < (numLeftSensors + 1) / 2) {
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
  
  if (validCount < (numRightSensors + 1) / 2) {
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
    if (leftTemperatures[i] > -100 && leftTemperatures[i] < 60) {
      sum += leftTemperatures[i];
      validCount++;
    }
  }
  
  // Add RIGHT sensors
  for (int i = 0; i < numRightSensors && i < 3; i++) {
    if (rightTemperatures[i] > -100 && rightTemperatures[i] < 60) {
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
  // Update target based on minimum
  TEMP_TARGET = TEMP_MIN + 2.0;
  
  // DO NOT operate if sensors are faulty
  if (heaterSensorFault || airSensorFault || averageTemp <= -100.0) {
    if (heaterOn || fansOn) {
      Serial.println("FAULT: Shutting down due to sensor failure");
      emergencyShutdown();
    }
    return;
  }
  
  // HEATING MODE: Average temp below minimum
  if (averageTemp < TEMP_MIN) {
    // FANS MUST RUN WHEN HEATING
    if (!fansOn) {
      fansOn = true;
      digitalWrite(FAN_RELAY_PIN, HIGH);
      Serial.println("Fans ON (Heating Mode)");
    }
    
    // Check if we can safely turn on heater
    if (!heaterOn && heaterTemp < HEATER_SAFETY_MAX - 2.0) {
      heaterOn = true;
      digitalWrite(HEATER_SSR_PIN, HIGH);
      Serial.print("HEATING MODE: ON | Avg: ");
      Serial.print(averageTemp, 1);
      Serial.print("C | Heater: ");
      Serial.print(heaterTemp, 1);
      Serial.println("C");
    }
    
    // Stop heating when target reached
    if (averageTemp >= TEMP_TARGET + HYSTERESIS && heaterOn) {
      heaterOn = false;
      digitalWrite(HEATER_SSR_PIN, LOW);
      fansOn = false;
      digitalWrite(FAN_RELAY_PIN, LOW);
      Serial.println("Target reached: Heater and fans OFF");
    }
    return;
  }
  
  // COOLING MODE: Average temp above maximum
  if (averageTemp > TEMP_MAX) {
    // Turn off heater if it's on
    if (heaterOn) {
      heaterOn = false;
      digitalWrite(HEATER_SSR_PIN, LOW);
    }
    
    // Turn on fans for cooling
    if (!fansOn) {
      fansOn = true;
      digitalWrite(FAN_RELAY_PIN, HIGH);
      Serial.print("COOLING MODE: Fans ON | Avg: ");
      Serial.print(averageTemp, 1);
      Serial.println("C");
    }
    
    // Turn off fans when cooled
    if (averageTemp <= TEMP_MAX - HYSTERESIS && fansOn) {
      fansOn = false;
      digitalWrite(FAN_RELAY_PIN, LOW);
      Serial.println("Cooling complete: Fans OFF");
    }
    return;
  }
  
  // IDLE MODE: Temperature in acceptable range
  if (heaterOn || fansOn) {
    heaterOn = false;
    fansOn = false;
    digitalWrite(HEATER_SSR_PIN, LOW);
    digitalWrite(FAN_RELAY_PIN, LOW);
    Serial.print("IDLE MODE | Avg: ");
    Serial.print(averageTemp, 1);
    Serial.println("C");
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
    Serial.println("C");
    
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
  // ========================================
  // FIRST TIME ONLY - Draw static elements
  // ========================================
  if (!displayInitialized) {
    tft.fillScreen(ST77XX_BLACK);
    
    // Header with box
    tft.drawRect(0, 0, 160, 25, ST77XX_GREEN);
    tft.setCursor(5, 3);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_GREEN);
    tft.println("GREENHOUSE v4.1");
    tft.setCursor(5, 13);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE);
    tft.print("IP:");
    tft.print(webServerIP);
    
    // Divider line
    tft.drawLine(0, 26, 160, 26, ST77XX_BLUE);
    
    // Labels for data sections
    tft.setCursor(5, 75);
    tft.setTextColor(ST77XX_CYAN);
    tft.print("Heater:");
    
    tft.setCursor(5, 95);
    tft.setTextColor(ST77XX_WHITE);
    tft.print("Status:");
    
    displayInitialized = true;
  }
  
  // ========================================
  // FAULT INDICATOR - Top priority
  // ========================================
  bool currentFaultState = (heaterSensorFault || airSensorFault);
  if (currentFaultState != lastFaultState) {
    tft.fillRect(5, 30, 150, 10, ST77XX_BLACK);
    if (currentFaultState) {
      tft.setCursor(5, 30);
      tft.setTextSize(1);
      tft.setTextColor(ST77XX_RED);
      tft.print("FAULT: ");
      if (heaterSensorFault) tft.print("HEATER ");
      if (airSensorFault) tft.print("AIR");
    }
    lastFaultState = currentFaultState;
  }
  
  // ========================================
  // AVERAGE TEMPERATURE - Large Display
  // ========================================
  if (abs(averageTemp - lastDisplayedAvg) > 0.1 || averageTemp <= -100) {
    // Clear area
    tft.fillRect(5, 45, 150, 25, ST77XX_BLACK);
    
    tft.setCursor(5, 48);
    tft.setTextSize(3);  // BIG!
    
    if (averageTemp > -100) {
      // Color based on temperature range
      if (averageTemp < TEMP_MIN) {
        tft.setTextColor(ST77XX_CYAN);  // Cold - heating needed
      } else if (averageTemp > TEMP_MAX) {
        tft.setTextColor(ST77XX_RED);   // Hot - cooling needed
      } else {
        tft.setTextColor(ST77XX_GREEN); // Perfect range
      }
      
      tft.print(averageTemp, 1);
      tft.setTextSize(2);
      tft.print("C");
    } else {
      tft.setTextColor(ST77XX_RED);
      tft.setTextSize(2);
      tft.print("ERROR");
    }
    
    lastDisplayedAvg = averageTemp;
  }
  
  // ========================================
  // HEATER SENSOR TEMPERATURE
  // ========================================
  if (abs(heaterTemp - lastDisplayedHeater) > 0.1) {
    tft.fillRect(60, 75, 95, 10, ST77XX_BLACK);
    
    tft.setCursor(60, 75);
    tft.setTextSize(1);
    
    if (heaterTemp > -100) {
      // Color based on safety margins
      if (heaterTemp >= HEATER_SAFETY_MAX - 2) {
        tft.setTextColor(ST77XX_RED);
      } else if (heaterTemp >= HEATER_SAFETY_MAX - 5) {
        tft.setTextColor(ST77XX_ORANGE);
      } else {
        tft.setTextColor(ST77XX_YELLOW);
      }
      tft.print(heaterTemp, 1);
      tft.print("C");
    } else {
      tft.setTextColor(ST77XX_RED);
      tft.print("FAULT");
    }
    
    lastDisplayedHeater = heaterTemp;
  }
  
  // ========================================
  // STATUS INDICATORS with boxes
  // ========================================
  if (heaterOn != lastHeaterState || fansOn != lastFanState) {
    tft.fillRect(5, 105, 150, 20, ST77XX_BLACK);
    
    // Heater status box
    tft.drawRect(5, 105, 70, 18, heaterOn ? ST77XX_RED : ST77XX_WHITE);
    tft.setCursor(15, 109);
    tft.setTextSize(1);
    tft.setTextColor(heaterOn ? ST77XX_RED : ST77XX_WHITE);
    tft.print("HEAT:");
    tft.print(heaterOn ? "ON" : "OFF");
    
    // Fan status box
    tft.drawRect(85, 105, 70, 18, fansOn ? ST77XX_GREEN : ST77XX_WHITE);
    tft.setCursor(95, 109);
    tft.setTextColor(fansOn ? ST77XX_GREEN : ST77XX_WHITE);
    tft.print("FAN:");
    tft.print(fansOn ? "ON" : "OFF");
    
    lastHeaterState = heaterOn;
    lastFanState = fansOn;
  }
}

void printAddress(DeviceAddress deviceAddress) {
  for (uint8_t i = 0; i < 8; i++) {
    if (deviceAddress[i] < 16) Serial.print("0");
    Serial.print(deviceAddress[i], HEX);
  }
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/adjust", handleAdjust);
  server.on("/status", handleStatusJSON);
}

void handleRoot() {
  server.send(200, "text/html", getHTMLPage());
}

void handleAdjust() {
  if (server.hasArg("param") && server.hasArg("action")) {
    String param = server.arg("param");
    String action = server.arg("action");
    float delta = (action == "up") ? 0.5 : -0.5;
    
    if (param == "tempmin") {
      TEMP_MIN += delta;
      TEMP_MIN = constrain(TEMP_MIN, 5.0, 15.0);
    } else if (param == "tempmax") {
      TEMP_MAX += delta;
      TEMP_MAX = constrain(TEMP_MAX, 20.0, 35.0);
    } else if (param == "heatmax") {
      HEATER_SAFETY_MAX += delta;
      HEATER_SAFETY_MAX = constrain(HEATER_SAFETY_MAX, 30.0, 40.0);
    }
    
    Serial.print("Adjusted ");
    Serial.print(param);
    Serial.print(" to ");
    if (param == "tempmin") Serial.println(TEMP_MIN);
    else if (param == "tempmax") Serial.println(TEMP_MAX);
    else if (param == "heatmax") Serial.println(HEATER_SAFETY_MAX);
  }
  
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleStatusJSON() {
  String json = "{";
  json += "\"avg\":" + String(averageTemp, 1) + ",";
  json += "\"heater\":" + String(heaterOn ? "1" : "0") + ",";
  json += "\"fans\":" + String(fansOn ? "1" : "0") + ",";
  json += "\"heat_temp\":" + String(heaterTemp, 1) + ",";
  json += "\"temp_min\":" + String(TEMP_MIN, 1) + ",";
  json += "\"temp_max\":" + String(TEMP_MAX, 1) + ",";
  json += "\"heater_max\":" + String(HEATER_SAFETY_MAX, 1) + ",";
  json += "\"left\":[";
  for (int i = 0; i < numLeftSensors && i < 3; i++) {
    if (i > 0) json += ",";
    json += String(leftTemperatures[i], 1);
  }
  json += "],\"right\":[";
  for (int i = 0; i < numRightSensors && i < 3; i++) {
    if (i > 0) json += ",";
    json += String(rightTemperatures[i], 1);
  }
  json += "]}";
  server.send(200, "application/json", json);
}

String getHTMLPage() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<style>";
  html += "body{font-family:Arial;margin:20px;background:#1a1a1a;color:#fff;}";
  html += ".container{max-width:600px;margin:0 auto;}";
  html += "h1{color:#4CAF50;text-align:center;}";
  html += ".card{background:#2a2a2a;padding:15px;margin:10px 0;border-radius:8px;}";
  html += ".temp{font-size:32px;font-weight:bold;color:#FFA500;text-align:center;}";
  html += ".status{display:inline-block;padding:8px 20px;border-radius:15px;margin:5px;font-weight:bold;}";
  html += ".status.on{background:#4CAF50;color:#000;}";
  html += ".status.off{background:#666;color:#ccc;}";
  html += ".control{display:flex;justify-content:space-between;align-items:center;margin:10px 0;padding:10px;background:#333;border-radius:5px;}";
  html += ".btn{background:#4CAF50;border:none;color:#000;padding:10px 20px;font-size:16px;font-weight:bold;border-radius:5px;cursor:pointer;}";
  html += ".value{font-size:18px;font-weight:bold;color:#4CAF50;}";
  html += ".sensor-grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px;text-align:center;}";
  html += ".sensor{background:#333;padding:10px;border-radius:5px;}";
  html += "</style>";
  
  html += "<script>";
  html += "async function updateStatus(){";
  html += "try{";
  html += "const r=await fetch('/status');";
  html += "const d=await r.json();";
  html += "document.getElementById('avgTemp').textContent=(d.avg>-100)?d.avg.toFixed(1)+'C':'ERR';";
  html += "document.getElementById('heaterState').textContent=(d.heater=='1')?'ON':'OFF';";
  html += "document.getElementById('heaterState').className='status '+(d.heater=='1'?'on':'off');";
  html += "document.getElementById('fansState').textContent=(d.fans=='1')?'ON':'OFF';";
  html += "document.getElementById('fansState').className='status '+(d.fans=='1'?'on':'off');";
  html += "document.getElementById('heatTemp').textContent=(d.heat_temp>-100)?d.heat_temp.toFixed(1)+'C':'ERR';";
  html += "document.getElementById('tempMinVal').textContent=d.temp_min.toFixed(1)+'C';";
  html += "document.getElementById('tempMaxVal').textContent=d.temp_max.toFixed(1)+'C';";
  html += "document.getElementById('heaterMaxVal').textContent=d.heater_max.toFixed(1)+'C';";
  html += "}catch(e){}}";
  html += "async function adjust(p,a){";
  html += "await fetch('/adjust?param='+p+'&action='+a);";
  html += "setTimeout(updateStatus,100);}";
  html += "setInterval(updateStatus,2000);";
  html += "window.onload=updateStatus;";
  html += "</script>";
  
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>Greenhouse Control</h1>";
  
  // Status Card
  html += "<div class='card'>";
  html += "<h2>Status</h2>";
  
  if (heaterSensorFault || airSensorFault) {
    html += "<div class='status' style='background:#f44336;'>FAULT</div><br>";
    if (heaterSensorFault) html += "<span style='color:#f44336;'>Heater Sensor Error</span><br>";
    if (airSensorFault) html += "<span style='color:#f44336;'>Air Sensors Error</span><br>";
  }
  
  html += "<div class='status " + String(heaterOn ? "on" : "off") + "' id='heaterState'>";
  html += "Heater: " + String(heaterOn ? "ON" : "OFF") + "</div>";
  
  html += "<div class='status " + String(fansOn ? "on" : "off") + "' id='fansState'>";
  html += "Fans: " + String(fansOn ? "ON" : "OFF") + "</div>";
  html += "</div>";
  
  // Temperature Card
  html += "<div class='card'>";
  html += "<h2>Temperatures</h2>";
  html += "<div class='temp' id='avgTemp'>" + String(averageTemp, 1) + "C</div>";
  html += "<p style='text-align:center;color:#aaa;'>Average Temperature</p>";
  html += "<p style='text-align:center;'>Heater Sensor: <span id='heatTemp' style='color:#FF6B6B;font-weight:bold;'>" + String(heaterTemp, 1) + "C</span></p>";
  html += "</div>";
  
  // Sensors Card
  html += "<div class='card'>";
  html += "<h2>Sensors</h2>";
  html += "<div class='sensor-grid'>";
  
  for (int i = 0; i < numLeftSensors && i < 3; i++) {
    html += "<div class='sensor'>";
    html += "<div style='color:#aaa;'>L" + String(i) + "</div>";
    if (leftTemperatures[i] > -100)
      html += "<div style='color:#87CEEB;font-weight:bold;'>" + String(leftTemperatures[i], 1) + "C</div>";
    else
      html += "<div style='color:#f44336;'>ERR</div>";
    html += "</div>";
  }
  
  for (int i = 0; i < numRightSensors && i < 3; i++) {
    html += "<div class='sensor'>";
    html += "<div style='color:#aaa;'>R" + String(i) + "</div>";
    if (rightTemperatures[i] > -100)
      html += "<div style='color:#87CEEB;font-weight:bold;'>" + String(rightTemperatures[i], 1) + "C</div>";
    else
      html += "<div style='color:#f44336;'>ERR</div>";
    html += "</div>";
  }
  
  html += "</div></div>";
  
  // Controls Card
  html += "<div class='card'>";
  html += "<h2>Controls</h2>";
  
  html += "<div class='control'>";
  html += "<span>Heating ON at:</span>";
  html += "<span class='value' id='tempMinVal'>" + String(TEMP_MIN, 1) + "C</span>";
  html += "<button class='btn' onclick='adjust(\"tempmin\",\"down\")'>-</button>";
  html += "<button class='btn' onclick='adjust(\"tempmin\",\"up\")'>+</button>";
  html += "</div>";
  
  html += "<div class='control'>";
  html += "<span>Cooling ON at:</span>";
  html += "<span class='value' id='tempMaxVal'>" + String(TEMP_MAX, 1) + "C</span>";
  html += "<button class='btn' onclick='adjust(\"tempmax\",\"down\")'>-</button>";
  html += "<button class='btn' onclick='adjust(\"tempmax\",\"up\")'>+</button>";
  html += "</div>";
  
  html += "<div class='control'>";
  html += "<span>Heater Max Temp:</span>";
  html += "<span class='value' id='heaterMaxVal'>" + String(HEATER_SAFETY_MAX, 1) + "C</span>";
  html += "<button class='btn' onclick='adjust(\"heatmax\",\"down\")'>-</button>";
  html += "<button class='btn' onclick='adjust(\"heatmax\",\"up\")'>+</button>";
  html += "</div>";
  
  html += "</div>";
  
  html += "<div style='text-align:center;margin-top:30px;color:#666;font-size:12px;'>";
  html += "Greenhouse Controller v4.1";
  html += "</div>";
  
  html += "</div></body></html>";
  return html;
}