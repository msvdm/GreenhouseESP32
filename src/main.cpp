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
// #define COOLING_FAN_PIN 27     // GPIO for 5V DC cooling fan via transistor (NOT IMPLEMENTED)

// TFT Display Pins (Hardware SPI)
#define TFT_CS    5
#define TFT_RST   17
#define TFT_DC    2            // Changed from 16 (now used for heater sensor)
#define TFT_SCLK  18           // SCK
#define TFT_MOSI  23           // SDA/MOSI

// PWM Configuration for Heater
#define HEATER_PWM_CHANNEL 0
#define HEATER_PWM_FREQ 1      // 1 Hz (slow for thermal mass)
#define HEATER_PWM_RESOLUTION 8 // 0-255

// Temperature Thresholds (now adjustable via web interface)
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
// bool coolingFanOn = true;  // NOT IMPLEMENTED
bool heaterSensorFault = false;
bool airSensorFault = false;
int heaterDutyCycle = 0; // 0-255 for PWM

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
void setupWebServer();
void handleRoot();
void handleAdjust();
String getHTMLPage();

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n=================================");
  Serial.println("GREENHOUSE CONTROLLER v4.0");
  Serial.println("THREE BUS + WEB INTERFACE");
  Serial.println("=================================");
  Serial.println("Bus 1 (GPIO 4):  LEFT sensors");
  Serial.println("Bus 2 (GPIO 15): RIGHT sensors");
  Serial.println("Bus 3 (GPIO 16): HEATER sensor");
  Serial.println("=================================\n");
  
  // Initialize pins
  pinMode(FAN_RELAY_PIN, OUTPUT);
  // pinMode(COOLING_FAN_PIN, OUTPUT);  // NOT IMPLEMENTED
  
  // Setup PWM for heater
  ledcSetup(HEATER_PWM_CHANNEL, HEATER_PWM_FREQ, HEATER_PWM_RESOLUTION);
  ledcAttachPin(HEATER_SSR_PIN, HEATER_PWM_CHANNEL);
  
  // Ensure everything starts OFF (SAFETY)
  ledcWrite(HEATER_PWM_CHANNEL, 0);
  digitalWrite(FAN_RELAY_PIN, LOW);
  // digitalWrite(COOLING_FAN_PIN, LOW);  // NOT IMPLEMENTED
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
  tft.println("3-Bus + Web UI");
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
  
  if (validCount <= numLeftSensors / 2) {
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
  
  if (validCount <= numRightSensors / 2) {
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
  
  // SINGLE clear logic path - require at least 1 working sensor
  if (validCount >= 1) {
    averageTemp = sum / validCount;
    airSensorFault = false;
  } else {
    averageTemp = -127.0;
    airSensorFault = true;
    Serial.print("WARNING: No air sensors working (");
    Serial.print(validCount);
    Serial.println(" available)");
  }
}

void controlSystem() {
  // Recalculate target based on minimum (your logic)
  TEMP_TARGET = TEMP_MIN + 2.0;

  // If average temperature is invalid -> immediate fault
  if (averageTemp <= -100.0) {
    Serial.println("ERROR: averageTemp invalid -> airSensorFault");
    airSensorFault = true;
    emergencyShutdown();
    return;
  }

  // If any critical sensor fault exists -> stop system
  if (heaterSensorFault || airSensorFault) {
    if (heaterOn || fansOn) {
      Serial.println("FAULT: Shutting down due to sensor failure");
      emergencyShutdown();
    }
    return;
  }

  //-----------------------------------------------------------------------
  //                             HEATING MODE
  //-----------------------------------------------------------------------
  if (averageTemp < TEMP_MIN) {

    // FANS MUST ALWAYS RUN WHEN HEATING
    if (!fansOn) {
      fansOn = true;
      digitalWrite(FAN_RELAY_PIN, HIGH);
      Serial.println("Fans ON (Heating Mode)");
    }

    // Heater sensor must be valid
    if (!heaterSensorDetected || heaterTemp <= -100.0) {
      Serial.println("SAFETY: Cannot heat without valid heater sensor");
      heaterOn = false;
      heaterDutyCycle = 0;
      ledcWrite(HEATER_PWM_CHANNEL, 0);
      return;
    }

    // Heater proportional control (SAFE FLOAT VERSION)
    float margin = HEATER_SAFETY_MAX - heaterTemp;

    if (margin >= 5.0) {
      // Clamp margin to the 5..15 range
      float c = margin;
      if (c < 5.0)  c = 5.0;
      if (c > 15.0) c = 15.0;

      // Linear interpolation: 5..15 → 128..255
      float ratio = (c - 5.0) / 10.0;  // 0..1
      int pwm = (int)(128 + ratio * (255 - 128) + 0.5);
      pwm = constrain(pwm, 0, 255);

      heaterDutyCycle = pwm;
      ledcWrite(HEATER_PWM_CHANNEL, heaterDutyCycle);
      heaterOn = true;

    } else {
      // Too close to limit → turn off
      heaterOn = false;
      heaterDutyCycle = 0;
      ledcWrite(HEATER_PWM_CHANNEL, 0);
    }

    // Stop heating once target is reached + hysteresis
    if (averageTemp >= TEMP_TARGET + HYSTERESIS) {
      heaterOn = false;
      heaterDutyCycle = 0;
      ledcWrite(HEATER_PWM_CHANNEL, 0);
      fansOn = false;
      digitalWrite(FAN_RELAY_PIN, LOW);
      Serial.println("Heating target reached → Heater + Fans OFF");
    }

    return; // we are in heating mode, stop here
  }

  //-----------------------------------------------------------------------
  //                              COOLING MODE
  //-----------------------------------------------------------------------
  if (averageTemp > TEMP_MAX) {

    // If heating is on → turn it off
    if (heaterOn) {
      heaterOn = false;
      heaterDutyCycle = 0;
      ledcWrite(HEATER_PWM_CHANNEL, 0);
    }

    // Turn ON fans for cooling
    if (!fansOn) {
      fansOn = true;
      digitalWrite(FAN_RELAY_PIN, HIGH);
      Serial.print("COOLING MODE: Fans ON | Avg=");
      Serial.print(averageTemp, 1);
      Serial.println("C");
    }

    // Turn OFF fans when cooled below threshold
    if (averageTemp <= TEMP_MAX - HYSTERESIS && fansOn) {
      fansOn = false;
      digitalWrite(FAN_RELAY_PIN, LOW);
      Serial.println("Cooling complete → Fans OFF");
    }

    return;
  }

  //-----------------------------------------------------------------------
  //                                 IDLE
  //-----------------------------------------------------------------------
  // Temperature is within acceptable range
  if (heaterOn || fansOn) {
    heaterOn = false;
    fansOn = false;
    heaterDutyCycle = 0;

    ledcWrite(HEATER_PWM_CHANNEL, 0);
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
      heaterDutyCycle = 0;
      ledcWrite(HEATER_PWM_CHANNEL, 0);
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
  heaterDutyCycle = 0;
  ledcWrite(HEATER_PWM_CHANNEL, 0);
  digitalWrite(FAN_RELAY_PIN, LOW);
  Serial.println("=== EMERGENCY SHUTDOWN ===");
}

void updateDisplay() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(0, 0);
  tft.setTextSize(1);
  
  // Title
  tft.setTextColor(ST77XX_GREEN);
  tft.println("GREENHOUSE WEB");
  tft.println("===============");
  
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
    tft.print("C PWM:");
    tft.println(heaterDutyCycle);
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

void handleStatusJSON() {
  String json = "{";
  json += "\"avg\":" + String(averageTemp, 1) + ",";
  json += "\"heater\":" + String(heaterOn ? "1" : "0") + ",";
  json += "\"fans\":" + String(fansOn ? "1" : "0") + ",";
  json += "\"heat_temp\":" + String(heaterTemp, 1) + ",";
  json += "\"pwm\":" + String(heaterDutyCycle) + ",";
  json += "\"temp_min\":" + String(TEMP_MIN, 1) + ",";
  json += "\"temp_max\":" + String(TEMP_MAX, 1) + ",";
  json += "\"heater_max\":" + String(HEATER_SAFETY_MAX, 1);
  json += "}";
  server.send(200, "application/json", json);
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
      TEMP_MAX = constrain(TEMP_MAX, 25.0, 35.0);
    } else if (param == "heatmax") {
      HEATER_SAFETY_MAX += delta;
      HEATER_SAFETY_MAX = constrain(HEATER_SAFETY_MAX, 20.0, 35.0);
    }
    
    Serial.print("Parameter adjusted: ");
    Serial.print(param);
    Serial.print(" ");
    Serial.print(action);
    Serial.print(" -> ");
    if (param == "tempmin") Serial.println(TEMP_MIN);
    else if (param == "tempmax") Serial.println(TEMP_MAX);
    else if (param == "heatmax") Serial.println(HEATER_SAFETY_MAX);
  }
  
  server.sendHeader("Location", "/");
  server.send(303);
}

String getHTMLPage() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";

  // STYLE
  html += "<style>";
  html += "body{font-family:Arial;margin:20px;background:#1a1a1a;color:#fff;}";
  html += ".container{max-width:600px;margin:0 auto;}";
  html += "h1{color:#4CAF50;text-align:center;}";
  html += ".card{background:#2a2a2a;padding:15px;margin:10px 0;border-radius:8px;border-left:4px solid #4CAF50;}";
  html += ".sensor-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin:10px 0;}";
  html += ".sensor{background:#333;padding:10px;border-radius:5px;text-align:center;}";
  html += ".temp{font-size:24px;font-weight:bold;color:#FFA500;}";
  html += ".label{font-size:12px;color:#aaa;}";
  html += ".status{display:inline-block;padding:5px 15px;border-radius:15px;margin:5px;font-weight:bold;}";
  html += ".status.on{background:#4CAF50;color:#000;}";
  html += ".status.off{background:#666;color:#ccc;}";
  html += ".status.fault{background:#f44336;color:#fff;}";
  html += ".control{display:flex;justify-content:space-between;align-items:center;margin:10px 0;padding:10px;background:#333;border-radius:5px;gap:10px;}";
  html += ".btn{background:#4CAF50;border:none;color:#000;padding:10px 20px;font-size:18px;font-weight:bold;border-radius:5px;cursor:pointer;margin:0 5px;}";
  html += ".btn:active{background:#45a049;}";
  html += ".value{font-size:20px;font-weight:bold;color:#4CAF50;min-width:60px;text-align:center;}";
  html += ".error{color:#f44336;}";
  html += "</style>";

  // JAVASCRIPT AUTO-UPDATE & BUTTON HANDLERS
  html += "<script>";
  html += "async function updateStatus(){";
  html += "try{";
  html += "const r=await fetch('/status');";
  html += "const d=await r.json();";
  html += "document.getElementById('avgTemp').textContent=(d.avg>-100)?d.avg.toFixed(1)+'C':'ERR';";
  html += "document.getElementById('heaterState').textContent=(d.heater=='1')?'ON':'OFF';";
  html += "document.getElementById('fansState').textContent=(d.fans=='1')?'ON':'OFF';";
  html += "document.getElementById('heatTemp').textContent=(d.heat_temp>-100)?d.heat_temp.toFixed(1)+'C':'ERR';";
  html += "document.getElementById('pwmVal').textContent=d.pwm+'/255';";
  html += "document.getElementById('tempMinVal').textContent=d.temp_min.toFixed(1)+'C';";
  html += "document.getElementById('tempMaxVal').textContent=d.temp_max.toFixed(1)+'C';";
  html += "document.getElementById('heaterMaxVal').textContent=d.heater_max.toFixed(1)+'C';";
  html += "}catch(e){ console.log('Update failed'); }}";
  html += "async function adjust(param,action){";
  html += "try{";
  html += "await fetch('/adjust?param='+param+'&action='+action);";
  html += "setTimeout(updateStatus, 100);";
  html += "}catch(e){ console.log('Adjust failed'); }}";
  html += "setInterval(updateStatus, 2000);";
  html += "window.onload = updateStatus;";
  html += "</script>";

  html += "</head><body>";

  html += "<div class='container'>";
  html += "<h1>Greenhouse Control</h1>";

  // STATUS CARD
  html += "<div class='card'>";
  html += "<h2 style='margin-top:0;'>System Status</h2>";

  if (heaterSensorFault || airSensorFault) {
    html += "<div class='status fault'>FAULT</div><br>";
    if (heaterSensorFault) html += "<span class='error'>Heater Sensor Error</span><br>";
    if (airSensorFault)   html += "<span class='error'>Air Sensors Error</span><br>";
  }

  html += "<div class='status " + String(heaterOn ? "on" : "off") + "'>";
  html += "Heater: <span id='heaterState'>" + String(heaterOn ? "ON" : "OFF") + "</span></div>";

  html += "<div class='status " + String(fansOn ? "on" : "off") + "'>";
  html += "Fans: <span id='fansState'>" + String(fansOn ? "ON" : "OFF") + "</span></div>";

  html += "<br><span class='label'>Heater PWM: <span id='pwmVal'>" + String(heaterDutyCycle) + "/255</span></span>";
  html += "</div>";

  // TEMPERATURE CARD
  html += "<div class='card'>";
  html += "<h2 style='margin-top:0;'>Temperatures</h2>";

  html += "<div style='text-align:center;margin:15px 0;'>";
  html += "<div class='label'>AVERAGE</div>";
  html += "<div class='temp'><span id='avgTemp'>" + String(averageTemp, 1) + "C</span></div>";
  html += "</div>";

  html += "<div style='text-align:center;margin:10px 0;'>";
  html += "<div class='label'>HEATER</div>";
  html += "<div class='temp' style='color:#FF6B6B;'><span id='heatTemp'>" + String(heaterTemp, 1) + "C</span></div>";
  html += "</div>";
  html += "</div>";

  // LEFT SENSORS
  html += "<div class='card'>";
  html += "<h2 style='margin-top:0;'>LEFT Sensors</h2>";
  html += "<div class='sensor-grid'>";
  for (int i = 0; i < numLeftSensors && i < 3; i++) {
    html += "<div class='sensor'>";
    html += "<div class='label'>L" + String(i) + "</div>";
    if (leftTemperatures[i] > -100)
      html += "<div class='temp' style='color:#87CEEB;'>" + String(leftTemperatures[i], 1) + "C</div>";
    else
      html += "<div class='temp error'>ERROR</div>";
    html += "</div>";
  }
  html += "</div></div>";

  // RIGHT SENSORS
  html += "<div class='card'>";
  html += "<h2 style='margin-top:0;'>RIGHT Sensors</h2>";
  html += "<div class='sensor-grid'>";
  for (int i = 0; i < numRightSensors && i < 3; i++) {
    html += "<div class='sensor'>";
    html += "<div class='label'>R" + String(i) + "</div>";
    if (rightTemperatures[i] > -100)
      html += "<div class='temp' style='color:#87CEEB;'>" + String(rightTemperatures[i], 1) + "C</div>";
    else
      html += "<div class='temp error'>ERROR</div>";
    html += "</div>";
  }
  html += "</div></div>";

  // CONTROLS
  html += "<div class='card'>";
  html += "<h2 style='margin-top:0;'>Controls</h2>";

  // HEATING THRESHOLD
  html += "<div class='control'>";
  html += "<span class='label' style='min-width:120px;'>Heating ON at:</span>";
  html += "<div class='value' id='tempMinVal' style='min-width:50px;'>" + String(TEMP_MIN, 1) + "C</div>";
  html += "<button class='btn' style='padding:8px 15px;font-size:14px;margin:0 2px;' onclick='adjust(\"tempmin\",\"up\"); return false;'>[+]</button>";
  html += "<button class='btn' style='padding:8px 15px;font-size:14px;margin:0 2px;' onclick='adjust(\"tempmin\",\"down\"); return false;'>[-]</button>";
  html += "</div>";

  // COOLING THRESHOLD
  html += "<div class='control'>";
  html += "<span class='label' style='min-width:120px;'>Cooling ON at:</span>";
  html += "<div class='value' id='tempMaxVal' style='min-width:50px;'>" + String(TEMP_MAX, 1) + "C</div>";
  html += "<button class='btn' style='padding:8px 15px;font-size:14px;margin:0 2px;' onclick='adjust(\"tempmax\",\"up\"); return false;'>[+]</button>";
  html += "<button class='btn' style='padding:8px 15px;font-size:14px;margin:0 2px;' onclick='adjust(\"tempmax\",\"down\"); return false;'>[-]</button>";
  html += "</div>";

  // HEATER MAX TEMP
  html += "<div class='control'>";
  html += "<span class='label' style='min-width:120px;'>Heater Max at:</span>";
  html += "<div class='value' id='heaterMaxVal' style='min-width:50px;'>" + String(HEATER_SAFETY_MAX, 1) + "C</div>";
  html += "<button class='btn' style='padding:8px 15px;font-size:14px;margin:0 2px;' onclick='adjust(\"heatmax\",\"up\"); return false;'>[+]</button>";
  html += "<button class='btn' style='padding:8px 15px;font-size:14px;margin:0 2px;' onclick='adjust(\"heatmax\",\"down\"); return false;'>[-]</button>";
  html += "</div>";

  html += "</div>"; // close card
  html += "</div>"; // close container

  html += "<div style='text-align:center;margin-top:30px;color:#666;font-size:12px;'>";
  html += "Greenhouse Controller v4.0 | Smooth UI via AJAX";
  html += "</div>";

  html += "</body></html>";
  return html;
}
