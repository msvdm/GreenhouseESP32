#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

// WiFi Credentials (Access Point Mode)
const char* ap_ssid = "Green";
const char* ap_password = "xD8ro6rNdcxbMWy!P78H";

// Pin Definitions - THREE SEPARATE BUSES
#define LEFT_SENSORS_BUS 4
#define RIGHT_SENSORS_BUS 15
#define HEATER_SENSOR_BUS 16
#define HEATER_SSR_PIN 25
#define FAN_RELAY_PIN 26

// TFT Display Pins
#define TFT_CS 5
#define TFT_RST 17
#define TFT_DC 2
#define TFT_SCLK 18
#define TFT_MOSI 23

// Temperature Thresholds
float TEMP_MIN = 10.0;
float TEMP_TARGET = 12.0;
float TEMP_MAX = 25.0;
float HEATER_SAFETY_MAX = 35.0;
#define HYSTERESIS 0.5
#define MAX_TEMP_CHANGE_PER_CYCLE 5.0  // Max °C change between readings

// Timing
#define AIR_TEMP_READ_INTERVAL 5000
#define HEATER_TEMP_READ_INTERVAL 1000
#define DISPLAY_UPDATE_INTERVAL 2000

// Setup OneWire buses
OneWire leftSensorsBus(LEFT_SENSORS_BUS);
OneWire rightSensorsBus(RIGHT_SENSORS_BUS);
OneWire heaterSensorBus(HEATER_SENSOR_BUS);

DallasTemperature leftSensors(&leftSensorsBus);
DallasTemperature rightSensors(&rightSensorsBus);
DallasTemperature heaterSensor(&heaterSensorBus);

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
WebServer server(80);

// Sensor addresses
DeviceAddress leftSensorAddresses[3];
DeviceAddress rightSensorAddresses[3];
DeviceAddress heaterSensorAddress;

int numLeftSensors = 0;
int numRightSensors = 0;
bool heaterSensorDetected = false;

// Temperature readings
float leftTemperatures[3] = {-127.0, -127.0, -127.0};
float rightTemperatures[3] = {-127.0, -127.0, -127.0};
float prevLeftTemperatures[3] = {-127.0, -127.0, -127.0};
float prevRightTemperatures[3] = {-127.0, -127.0, -127.0};
float averageTemp = -127.0;
float heaterTemp = -127.0;
float prevHeaterTemp = -127.0;

// System state
bool heaterOn = false;
bool fansOn = false;

// Display state tracking
bool displayInitialized = false;
float lastDisplayedAvg = -999.0;
String webServerIP = "192.168.4.1";

// Timing
unsigned long lastAirTempRead = 0;
unsigned long lastHeaterTempRead = 0;
unsigned long lastDisplayUpdate = 0;

// Forward declarations
void readHeaterTemperature();
void readAirTemperatures();
void calculateAverage();
void controlSystem();
void safetyShutdown();
void updateDisplay();
void handleRoot();
void handleAdjust();
void handleStatusJSON();

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println(F("\n=== GREENHOUSE CONTROLLER v4.2 ==="));
  
  // Initialize outputs to SAFE state
  pinMode(HEATER_SSR_PIN, OUTPUT);
  pinMode(FAN_RELAY_PIN, OUTPUT);
  digitalWrite(HEATER_SSR_PIN, LOW);
  digitalWrite(FAN_RELAY_PIN, LOW);
  
  // Configure watchdog
  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);
  
  Serial.println(F("Safety: All outputs OFF"));
  
  // Initialize LEFT sensors
  Serial.println(F("\n[Bus 1] LEFT sensors (GPIO 4)"));
  leftSensors.begin();
  leftSensors.setWaitForConversion(false);
  numLeftSensors = leftSensors.getDeviceCount();
  Serial.print(F("  Found: "));
  Serial.println(numLeftSensors);
  
  for (int i = 0; i < numLeftSensors && i < 3; i++) {
    if (leftSensors.getAddress(leftSensorAddresses[i], i)) {
      leftSensors.setResolution(leftSensorAddresses[i], 12);
      Serial.print(F("  L"));
      Serial.print(i);
      Serial.println(F(" OK"));
    }
  }
  
  // Initialize RIGHT sensors
  Serial.println(F("\n[Bus 2] RIGHT sensors (GPIO 15)"));
  rightSensors.begin();
  rightSensors.setWaitForConversion(false);
  numRightSensors = rightSensors.getDeviceCount();
  Serial.print(F("  Found: "));
  Serial.println(numRightSensors);
  
  for (int i = 0; i < numRightSensors && i < 3; i++) {
    if (rightSensors.getAddress(rightSensorAddresses[i], i)) {
      rightSensors.setResolution(rightSensorAddresses[i], 12);
      Serial.print(F("  R"));
      Serial.print(i);
      Serial.println(F(" OK"));
    }
  }
  
  // Initialize HEATER sensor
  Serial.println(F("\n[Bus 3] HEATER sensor (GPIO 16)"));
  heaterSensor.begin();
  heaterSensor.setWaitForConversion(false);
  int heaterSensorCount = heaterSensor.getDeviceCount();
  Serial.print(F("  Found: "));
  Serial.println(heaterSensorCount);
  
  if (heaterSensorCount == 1 && heaterSensor.getAddress(heaterSensorAddress, 0)) {
    heaterSensor.setResolution(heaterSensorAddress, 12);
    heaterSensorDetected = true;
    Serial.println(F("  Heater sensor OK"));
  } else {
    Serial.println(F("  ERROR: Heater sensor missing!"));
    Serial.println(F("  HEATING DISABLED"));
  }
  
  // Initialize display
  Serial.println(F("\nInitializing display..."));
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(0, 0);
  tft.println(F("Greenhouse v4.2"));
  tft.print(F("L:"));
  tft.print(numLeftSensors);
  tft.print(F(" R:"));
  tft.print(numRightSensors);
  tft.print(F(" H:"));
  tft.println(heaterSensorDetected ? "1" : "0");
  
  // Setup WiFi
  Serial.println(F("\nStarting WiFi AP..."));
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  
  IPAddress IP = WiFi.softAPIP();
  webServerIP = IP.toString();
  Serial.print(F("IP: "));
  Serial.println(IP);
  
  tft.print(F("IP: "));
  tft.println(IP);
  
  // Setup web server
  server.on("/", handleRoot);
  server.on("/adjust", handleAdjust);
  server.on("/status", handleStatusJSON);
  server.begin();
  
  delay(2000);
  Serial.println(F("\n=== System Ready ===\n"));
}

void loop() {
  unsigned long currentMillis = millis();
  
  esp_task_wdt_reset();
  server.handleClient();
  
  // Read heater temp frequently (SAFETY)
  if (currentMillis - lastHeaterTempRead >= HEATER_TEMP_READ_INTERVAL) {
    lastHeaterTempRead = currentMillis;
    readHeaterTemperature();
  }
  
  // Read air temps less frequently
  if (currentMillis - lastAirTempRead >= AIR_TEMP_READ_INTERVAL) {
    lastAirTempRead = currentMillis;
    readAirTemperatures();
    calculateAverage();
    controlSystem();
  }
  
  // Update display
  if (currentMillis - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
    lastDisplayUpdate = currentMillis;
    updateDisplay();
  }
  
  delay(10);
}

void readHeaterTemperature() {
  if (!heaterSensorDetected) {
    heaterTemp = -127.0;
    return;
  }
  
  heaterSensor.requestTemperatures();
  float temp = heaterSensor.getTempC(heaterSensorAddress);
  
  // Validate reading
  if (temp == DEVICE_DISCONNECTED_C || temp < -50 || temp > 100) {
    Serial.println(F("HEATER SENSOR ERROR"));
    heaterTemp = -127.0;
    safetyShutdown();
    return;
  }
  
  // Rate of change check
  if (prevHeaterTemp > -100) {
    float change = abs(temp - prevHeaterTemp);
    if (change > MAX_TEMP_CHANGE_PER_CYCLE) {
      Serial.print(F("HEATER SENSOR: Invalid change: "));
      Serial.println(change);
      return; // Keep previous reading
    }
  }
  
  prevHeaterTemp = heaterTemp;
  heaterTemp = temp;
  
  // Safety check - immediate shutdown if too hot
  if (heaterTemp >= HEATER_SAFETY_MAX) {
    Serial.print(F("CRITICAL: Heater temp: "));
    Serial.print(heaterTemp);
    Serial.println(F("C - SHUTDOWN"));
    safetyShutdown();
  }
}

void readAirTemperatures() {
  // Read LEFT sensors
  leftSensors.requestTemperatures();
  for (int i = 0; i < numLeftSensors && i < 3; i++) {
    float temp = leftSensors.getTempC(leftSensorAddresses[i]);
    
    if (temp != DEVICE_DISCONNECTED_C && temp > -50 && temp < 60) {
      // Rate of change check
      if (prevLeftTemperatures[i] > -100) {
        float change = abs(temp - prevLeftTemperatures[i]);
        if (change > MAX_TEMP_CHANGE_PER_CYCLE) {
          continue; // Skip this reading
        }
      }
      prevLeftTemperatures[i] = leftTemperatures[i];
      leftTemperatures[i] = temp;
    } else {
      leftTemperatures[i] = -127.0;
    }
  }
  
  // Read RIGHT sensors
  rightSensors.requestTemperatures();
  for (int i = 0; i < numRightSensors && i < 3; i++) {
    float temp = rightSensors.getTempC(rightSensorAddresses[i]);
    
    if (temp != DEVICE_DISCONNECTED_C && temp > -50 && temp < 60) {
      // Rate of change check
      if (prevRightTemperatures[i] > -100) {
        float change = abs(temp - prevRightTemperatures[i]);
        if (change > MAX_TEMP_CHANGE_PER_CYCLE) {
          continue; // Skip this reading
        }
      }
      prevRightTemperatures[i] = rightTemperatures[i];
      rightTemperatures[i] = temp;
    } else {
      rightTemperatures[i] = -127.0;
    }
  }
}

void calculateAverage() {
  float sum = 0.0;
  int validCount = 0;
  
  for (int i = 0; i < numLeftSensors && i < 3; i++) {
    if (leftTemperatures[i] > -100) {
      sum += leftTemperatures[i];
      validCount++;
    }
  }
  
  for (int i = 0; i < numRightSensors && i < 3; i++) {
    if (rightTemperatures[i] > -100) {
      sum += rightTemperatures[i];
      validCount++;
    }
  }
  
  if (validCount >= 1) {
    averageTemp = sum / validCount;
  } else {
    averageTemp = -127.0;
    Serial.println(F("ERROR: No valid air sensors"));
    safetyShutdown();
  }
}

void controlSystem() {
  TEMP_TARGET = TEMP_MIN + 2.0;
  
  // SAFETY CHECK: Cannot operate without valid sensors
  if (averageTemp <= -100.0 || heaterTemp <= -100.0 || !heaterSensorDetected) {
    if (heaterOn || fansOn) {
      safetyShutdown();
    }
    return;
  }
  
  // HEATING MODE
  if (averageTemp < TEMP_MIN) {
    // CRITICAL: Fans MUST be on before heater
    if (!fansOn) {
      fansOn = true;
      digitalWrite(FAN_RELAY_PIN, HIGH);
      Serial.println(F("Fans ON (heating prep)"));
      delay(100); // Let fans start
    }
    
    // CRITICAL: Only turn on heater if ALL conditions safe
    if (!heaterOn && 
        heaterTemp > 0 && 
        heaterTemp < HEATER_SAFETY_MAX - 2.0 && 
        fansOn) {
      heaterOn = true;
      digitalWrite(HEATER_SSR_PIN, HIGH);
      Serial.print(F("HEATING: Avg="));
      Serial.print(averageTemp, 1);
      Serial.print(F("C Heater="));
      Serial.print(heaterTemp, 1);
      Serial.println(F("C"));
    }
    
    // Stop heating when target reached
    if (averageTemp >= TEMP_TARGET + HYSTERESIS) {
      if (heaterOn || fansOn) {
        heaterOn = false;
        fansOn = false;
        digitalWrite(HEATER_SSR_PIN, LOW);
        digitalWrite(FAN_RELAY_PIN, LOW);
        Serial.println(F("Target reached - OFF"));
      }
    }
    return;
  }
  
  // COOLING MODE
  if (averageTemp > TEMP_MAX) {
    if (heaterOn) {
      heaterOn = false;
      digitalWrite(HEATER_SSR_PIN, LOW);
    }
    
    if (!fansOn) {
      fansOn = true;
      digitalWrite(FAN_RELAY_PIN, HIGH);
      Serial.print(F("COOLING: "));
      Serial.print(averageTemp, 1);
      Serial.println(F("C"));
    }
    
    if (averageTemp <= TEMP_MAX - HYSTERESIS) {
      fansOn = false;
      digitalWrite(FAN_RELAY_PIN, LOW);
      Serial.println(F("Cooling complete"));
    }
    return;
  }
  
  // IDLE MODE
  if (heaterOn || fansOn) {
    heaterOn = false;
    fansOn = false;
    digitalWrite(HEATER_SSR_PIN, LOW);
    digitalWrite(FAN_RELAY_PIN, LOW);
    Serial.print(F("IDLE: "));
    Serial.print(averageTemp, 1);
    Serial.println(F("C"));
  }
}

void safetyShutdown() {
  heaterOn = false;
  fansOn = false;
  digitalWrite(HEATER_SSR_PIN, LOW);
  digitalWrite(FAN_RELAY_PIN, LOW);
  Serial.println(F("=== SAFETY SHUTDOWN ==="));
}

void updateDisplay() {
  if (!displayInitialized) {
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(5, 5);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_GREEN);
    tft.print(F("IP: "));
    tft.println(webServerIP);
    tft.println(F("================"));
    displayInitialized = true;
  }
  
  // Error indicator
  bool hasError = (heaterTemp <= -100.0 || averageTemp <= -100.0 || !heaterSensorDetected);
  if (hasError) {
    tft.fillRect(0, 20, 160, 20, ST77XX_BLACK);
    tft.setCursor(5, 20);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_RED);
    tft.println(F("SENSOR ERROR"));
  }
  
  // Average temperature
  if (abs(averageTemp - lastDisplayedAvg) > 0.1 || !hasError) {
    tft.fillRect(0, 40, 160, 20, ST77XX_BLACK);
    tft.setCursor(1, 40);
    tft.setTextSize(2);
    
    if (averageTemp > -100) {
      if (averageTemp < TEMP_MIN) {
        tft.setTextColor(ST77XX_CYAN);
      } else if (averageTemp > TEMP_MAX) {
        tft.setTextColor(ST77XX_ORANGE);
      } else {
        tft.setTextColor(ST77XX_YELLOW);
      }
      tft.print(averageTemp, 1);
      tft.print(F("C"));
    } else {
      tft.setTextColor(ST77XX_RED);
      tft.print(F("ERROR"));
    }
    lastDisplayedAvg = averageTemp;
  }
  
  // Heater status
  tft.fillRect(0, 65, 160, 12, ST77XX_BLACK);
  tft.setCursor(2, 65);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_RED);
  if (heaterTemp > -100) {
    tft.print(F("H:"));
    tft.print(heaterTemp, 1);
    tft.print(F("C "));
  } else {
    tft.print(F("H:ERR "));
  }
  tft.print(heaterOn ? F("ON") : F("OFF"));
  
  // Fan status
  tft.fillRect(0, 80, 160, 12, ST77XX_BLACK);
  tft.setCursor(2, 80);
  tft.setTextColor(ST77XX_WHITE);
  tft.print(F("Fan: "));
  tft.print(fansOn ? F("ON") : F("OFF"));
  
  // Sensor values
  tft.fillRect(0, 95, 160, 33, ST77XX_BLACK);
  tft.setCursor(2, 95);
  tft.setTextColor(ST77XX_CYAN);
  
  for (int i = 0; i < numLeftSensors && i < 3; i++) {
    if (leftTemperatures[i] > -100) {
      tft.print(leftTemperatures[i], 1);
      tft.print(F(" "));
    } else {
      tft.print(F("-- "));
    }
  }
  
  tft.setCursor(2, 107);
  for (int i = 0; i < numRightSensors && i < 3; i++) {
    if (rightTemperatures[i] > -100) {
      tft.print(rightTemperatures[i], 1);
      tft.print(F(" "));
    } else {
      tft.print(F("-- "));
    }
  }
}

void handleRoot() {
  String html = F("<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{font-family:Arial;margin:20px;background:#1a1a1a;color:#fff;}"
    ".container{max-width:600px;margin:0 auto;}"
    "h1{color:#4CAF50;text-align:center;}"
    ".card{background:#2a2a2a;padding:15px;margin:10px 0;border-radius:8px;}"
    ".temp{font-size:32px;font-weight:bold;color:#FFA500;text-align:center;}"
    ".status{display:inline-block;padding:8px 20px;border-radius:15px;margin:5px;font-weight:bold;}"
    ".status.on{background:#4CAF50;color:#000;}"
    ".status.off{background:#666;color:#ccc;}"
    ".control{display:flex;justify-content:space-between;align-items:center;"
    "margin:10px 0;padding:10px;background:#333;border-radius:5px;}"
    ".btn{background:#4CAF50;border:none;color:#000;padding:10px 20px;"
    "font-size:16px;font-weight:bold;border-radius:5px;cursor:pointer;}"
    ".value{font-size:18px;font-weight:bold;color:#4CAF50;}"
    ".sensor-grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px;text-align:center;}"
    ".sensor{background:#333;padding:10px;border-radius:5px;}"
    "</style>"
    "<script>"
    "async function update(){"
    "try{"
    "const r=await fetch('/status');"
    "const d=await r.json();"
    "document.getElementById('avgTemp').textContent=(d.avg>-100)?d.avg.toFixed(1)+'C':'ERR';"
    "document.getElementById('heaterState').textContent=(d.heater==1)?'ON':'OFF';"
    "document.getElementById('heaterState').className='status '+((d.heater==1)?'on':'off');"
    "document.getElementById('fansState').textContent=(d.fans==1)?'ON':'OFF';"
    "document.getElementById('fansState').className='status '+((d.fans==1)?'on':'off');"
    "document.getElementById('heatTemp').textContent=(d.heat_temp>-100)?d.heat_temp.toFixed(1)+'C':'ERR';"
    "document.getElementById('tempMinVal').textContent=d.temp_min.toFixed(1)+'C';"
    "document.getElementById('tempMaxVal').textContent=d.temp_max.toFixed(1)+'C';"
    "document.getElementById('heaterMaxVal').textContent=d.heater_max.toFixed(1)+'C';"
    "}catch(e){}}"
    "async function adjust(p,a){"
    "await fetch('/adjust?param='+p+'&action='+a);"
    "setTimeout(update,100);}"
    "setInterval(update,2000);"
    "window.onload=update;"
    "</script>"
    "</head><body>"
    "<div class='container'>"
    "<h1>Greenhouse Control</h1>");
  
  html += F("<div class='card'><h2>Status</h2>");
  
  if (!heaterSensorDetected || heaterTemp <= -100.0 || averageTemp <= -100.0) {
    html += F("<div class='status' style='background:#f44336;'>FAULT</div><br>");
  }
  
  html += F("<div class='status ");
  html += heaterOn ? F("on") : F("off");
  html += F("' id='heaterState'>Heater: ");
  html += heaterOn ? F("ON") : F("OFF");
  html += F("</div>");
  
  html += F("<div class='status ");
  html += fansOn ? F("on") : F("off");
  html += F("' id='fansState'>Fans: ");
  html += fansOn ? F("ON") : F("OFF");
  html += F("</div></div>");
  
  html += F("<div class='card'><h2>Temperatures</h2>"
    "<div class='temp' id='avgTemp'>");
  html += String(averageTemp, 1);
  html += F("C</div>"
    "<p style='text-align:center;color:#aaa;'>Average Temperature</p>"
    "<p style='text-align:center;'>Heater: <span id='heatTemp' style='color:#FF6B6B;font-weight:bold;'>");
  html += String(heaterTemp, 1);
  html += F("C</span></p></div>");
  
  html += F("<div class='card'><h2>Sensors</h2><div class='sensor-grid'>");
  
  for (int i = 0; i < numLeftSensors && i < 3; i++) {
    html += F("<div class='sensor'><div style='color:#aaa;'>L");
    html += String(i);
    html += F("</div>");
    if (leftTemperatures[i] > -100) {
      html += F("<div style='color:#87CEEB;font-weight:bold;'>");
      html += String(leftTemperatures[i], 1);
      html += F("C</div>");
    } else {
      html += F("<div style='color:#f44336;'>ERR</div>");
    }
    html += F("</div>");
  }
  
  for (int i = 0; i < numRightSensors && i < 3; i++) {
    html += F("<div class='sensor'><div style='color:#aaa;'>R");
    html += String(i);
    html += F("</div>");
    if (rightTemperatures[i] > -100) {
      html += F("<div style='color:#87CEEB;font-weight:bold;'>");
      html += String(rightTemperatures[i], 1);
      html += F("C</div>");
    } else {
      html += F("<div style='color:#f44336;'>ERR</div>");
    }
    html += F("</div>");
  }
  
  html += F("</div></div>");
  
  html += F("<div class='card'><h2>Controls</h2>"
    "<div class='control'><span>Heating ON at:</span>"
    "<span class='value' id='tempMinVal'>");
  html += String(TEMP_MIN, 1);
  html += F("C</span>"
    "<button class='btn' onclick='adjust(\"tempmin\",\"down\")'>-</button>"
    "<button class='btn' onclick='adjust(\"tempmin\",\"up\")'>+</button></div>"
    "<div class='control'><span>Cooling ON at:</span>"
    "<span class='value' id='tempMaxVal'>");
  html += String(TEMP_MAX, 1);
  html += F("C</span>"
    "<button class='btn' onclick='adjust(\"tempmax\",\"down\")'>-</button>"
    "<button class='btn' onclick='adjust(\"tempmax\",\"up\")'>+</button></div>"
    "<div class='control'><span>Heater Max:</span>"
    "<span class='value' id='heaterMaxVal'>");
  html += String(HEATER_SAFETY_MAX, 1);
  html += F("C</span>"
    "<button class='btn' onclick='adjust(\"heatmax\",\"down\")'>-</button>"
    "<button class='btn' onclick='adjust(\"heatmax\",\"up\")'>+</button></div></div>"
    "<div style='text-align:center;margin-top:30px;color:#666;font-size:12px;'>"
    "Greenhouse Controller v4.2</div></div></body></html>");
  
  server.send(200, F("text/html"), html);
}

void handleAdjust() {
  if (server.hasArg(F("param")) && server.hasArg(F("action"))) {
    String param = server.arg(F("param"));
    String action = server.arg(F("action"));
    float delta = (action == F("up")) ? 0.5 : -0.5;
    
    if (param == F("tempmin")) {
      TEMP_MIN += delta;
      TEMP_MIN = constrain(TEMP_MIN, 5.0, 15.0);
    } else if (param == F("tempmax")) {
      TEMP_MAX += delta;
      TEMP_MAX = constrain(TEMP_MAX, 20.0, 35.0);
    } else if (param == F("heatmax")) {
      HEATER_SAFETY_MAX += delta;
      HEATER_SAFETY_MAX = constrain(HEATER_SAFETY_MAX, 30.0, 40.0);
    }
  }
  
  server.sendHeader(F("Location"), F("/"));
  server.send(303);
}

void handleStatusJSON() {
  StaticJsonDocument<256> doc;
  
  doc["avg"] = (averageTemp > -100) ? averageTemp : -127.0;
  doc["heater"] = heaterOn ? 1 : 0;
  doc["fans"] = fansOn ? 1 : 0;
  doc["heat_temp"] = (heaterTemp > -100) ? heaterTemp : -127.0;
  doc["temp_min"] = TEMP_MIN;
  doc["temp_max"] = TEMP_MAX;
  doc["heater_max"] = HEATER_SAFETY_MAX;
  
  JsonArray left = doc.createNestedArray("left");
  for (int i = 0; i < numLeftSensors && i < 3; i++) {
    left.add(leftTemperatures[i]);
  }
  
  JsonArray right = doc.createNestedArray("right");
  for (int i = 0; i < numRightSensors && i < 3; i++) {
    right.add(rightTemperatures[i]);
  }
  
  String output;
  serializeJson(doc, output);
  server.send(200, F("application/json"), output);
}