#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

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
#define MODE_CHANGE_HYSTERESIS 1.0     // Extra hysteresis to prevent mode switching

// Timing
#define AIR_TEMP_READ_INTERVAL 5000
#define HEATER_TEMP_READ_INTERVAL 1000
#define DISPLAY_UPDATE_INTERVAL 2000
#define FAN_COOLDOWN_TIME 45000        // Keep fans on 45s after heater off

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
unsigned long heaterOffTime = 0;       // Track when heater was turned off
unsigned long fanCooldownStart = 0;    // Track fan cooldown period
bool inCooldownMode = false;           // Flag for post-heating cooldown

// System mode tracking (for hysteresis)
enum SystemMode {
  MODE_IDLE,
  MODE_HEATING,
  MODE_COOLING,
  MODE_COOLDOWN
};
SystemMode currentMode = MODE_IDLE;

// Mutex for thread-safe access to shared variables
SemaphoreHandle_t dataMutex;

// Display state tracking
bool displayInitialized = false;
float lastDisplayedAvg = -999.0;
String webServerIP = "192.168.4.1";

// Timing - Initialize to current time to avoid millis overflow issues
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

  Serial.println(F("\n=== GREENHOUSE CONTROLLER v4.3 ==="));

  // Create mutex for thread-safe data access
  dataMutex = xSemaphoreCreateMutex();

  // Initialize outputs to SAFE state
  pinMode(HEATER_SSR_PIN, OUTPUT);
  pinMode(FAN_RELAY_PIN, OUTPUT);
  digitalWrite(HEATER_SSR_PIN, LOW);
  digitalWrite(FAN_RELAY_PIN, LOW);

  // Configure watchdog with shorter timeout for better safety
  esp_task_wdt_init(15, true);
  esp_task_wdt_add(NULL);

  Serial.println(F("Safety: All outputs OFF"));

  // Initialize timing variables to current millis to prevent overflow issues
  unsigned long now = millis();
  lastAirTempRead = now;
  lastHeaterTempRead = now;
  lastDisplayUpdate = now;
  
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
  tft.println(F("Greenhouse v4.3"));
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

  // Handle web server requests (brief, non-blocking)
  server.handleClient();

  // Read heater temp frequently (SAFETY CRITICAL)
  if (currentMillis - lastHeaterTempRead >= HEATER_TEMP_READ_INTERVAL) {
    lastHeaterTempRead = currentMillis;

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      readHeaterTemperature();
      xSemaphoreGive(dataMutex);
    }

    // Reset watchdog after critical safety check
    esp_task_wdt_reset();
  }

  // Read air temps less frequently
  if (currentMillis - lastAirTempRead >= AIR_TEMP_READ_INTERVAL) {
    lastAirTempRead = currentMillis;

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      readAirTemperatures();
      calculateAverage();
      controlSystem();
      xSemaphoreGive(dataMutex);
    }

    // Reset watchdog after control logic
    esp_task_wdt_reset();
  }

  // Update display (can be slow, don't hold mutex too long)
  if (currentMillis - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
    lastDisplayUpdate = currentMillis;
    updateDisplay();

    // Reset watchdog after display update
    esp_task_wdt_reset();
  }

  // Small delay to prevent tight looping
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
  unsigned long currentMillis = millis();
  TEMP_TARGET = TEMP_MIN + 2.0;

  // SAFETY CHECK: Cannot operate without valid sensors
  if (averageTemp <= -100.0 || heaterTemp <= -100.0 || !heaterSensorDetected) {
    if (heaterOn || fansOn) {
      safetyShutdown();
      currentMode = MODE_IDLE;
    }
    return;
  }

  // CRITICAL: Handle fan cooldown period after heating
  if (inCooldownMode) {
    unsigned long cooldownElapsed = currentMillis - fanCooldownStart;

    // Keep heater OFF during cooldown
    if (heaterOn) {
      heaterOn = false;
      digitalWrite(HEATER_SSR_PIN, LOW);
      Serial.println(F("Heater OFF (cooldown)"));
    }

    // Continue running fans during cooldown
    if (!fansOn) {
      fansOn = true;
      digitalWrite(FAN_RELAY_PIN, HIGH);
      Serial.println(F("Fans ON (cooldown)"));
    }

    // Check if cooldown is complete
    if (cooldownElapsed >= FAN_COOLDOWN_TIME) {
      inCooldownMode = false;
      fansOn = false;
      digitalWrite(FAN_RELAY_PIN, LOW);
      currentMode = MODE_IDLE;
      Serial.println(F("Cooldown complete - System IDLE"));
    }

    // Emergency: If temp gets too high during cooldown, enter cooling mode
    if (averageTemp > TEMP_MAX + MODE_CHANGE_HYSTERESIS) {
      inCooldownMode = false;
      currentMode = MODE_COOLING;
      Serial.println(F("Cooldown aborted - Switching to COOLING"));
    }

    return;
  }

  // Determine what mode we should be in based on temperature with hysteresis
  SystemMode targetMode = MODE_IDLE;

  if (averageTemp < TEMP_MIN - (currentMode == MODE_HEATING ? 0 : MODE_CHANGE_HYSTERESIS)) {
    targetMode = MODE_HEATING;
  } else if (averageTemp > TEMP_MAX + (currentMode == MODE_COOLING ? 0 : MODE_CHANGE_HYSTERESIS)) {
    targetMode = MODE_COOLING;
  } else if (averageTemp >= TEMP_TARGET + HYSTERESIS && currentMode == MODE_HEATING) {
    // Heating target reached - enter cooldown
    targetMode = MODE_COOLDOWN;
  } else if (averageTemp <= TEMP_MAX - HYSTERESIS && currentMode == MODE_COOLING) {
    // Cooling target reached
    targetMode = MODE_IDLE;
  } else {
    // Stay in current mode if within hysteresis range
    targetMode = currentMode;
  }

  // Handle mode transitions
  if (targetMode != currentMode) {
    Serial.print(F("Mode change: "));
    Serial.print(currentMode);
    Serial.print(F(" -> "));
    Serial.println(targetMode);
  }

  // Execute control logic based on target mode
  switch (targetMode) {
    case MODE_HEATING:
      // CRITICAL: Fans MUST be on before heater
      if (!fansOn) {
        fansOn = true;
        digitalWrite(FAN_RELAY_PIN, HIGH);
        Serial.println(F("Fans ON (heating prep)"));
        delay(100); // Let fans start spinning
      }

      // CRITICAL: Only turn on heater if ALL conditions safe
      if (!heaterOn &&
          heaterTemp > 0 &&
          heaterTemp < HEATER_SAFETY_MAX - 2.0 &&
          fansOn) {
        heaterOn = true;
        digitalWrite(HEATER_SSR_PIN, HIGH);
        Serial.print(F("Heater ON: Avg="));
        Serial.print(averageTemp, 1);
        Serial.print(F("C Heater="));
        Serial.print(heaterTemp, 1);
        Serial.println(F("C"));
      }
      currentMode = MODE_HEATING;
      break;

    case MODE_COOLING:
      // Turn off heater if it's on
      if (heaterOn) {
        heaterOn = false;
        digitalWrite(HEATER_SSR_PIN, LOW);
        Serial.println(F("Heater OFF (cooling mode)"));
      }

      // Turn on fans for cooling
      if (!fansOn) {
        fansOn = true;
        digitalWrite(FAN_RELAY_PIN, HIGH);
        Serial.print(F("COOLING: "));
        Serial.print(averageTemp, 1);
        Serial.println(F("C"));
        delay(50); // Brief delay for fan startup
      }
      currentMode = MODE_COOLING;
      break;

    case MODE_COOLDOWN:
      // Enter cooldown mode - heater off, fans stay on
      if (heaterOn) {
        heaterOn = false;
        digitalWrite(HEATER_SSR_PIN, LOW);
        heaterOffTime = currentMillis;
        Serial.println(F("Heater OFF - Starting cooldown"));
      }

      inCooldownMode = true;
      fanCooldownStart = currentMillis;
      currentMode = MODE_COOLDOWN;
      Serial.print(F("COOLDOWN started ("));
      Serial.print(FAN_COOLDOWN_TIME / 1000);
      Serial.println(F("s)"));
      break;

    case MODE_IDLE:
      // Turn everything off
      if (heaterOn) {
        heaterOn = false;
        digitalWrite(HEATER_SSR_PIN, LOW);
        Serial.println(F("Heater OFF (idle)"));
      }
      if (fansOn) {
        fansOn = false;
        digitalWrite(FAN_RELAY_PIN, LOW);
        Serial.println(F("Fans OFF (idle)"));
      }
      if (currentMode != MODE_IDLE) {
        Serial.print(F("IDLE: "));
        Serial.print(averageTemp, 1);
        Serial.println(F("C"));
      }
      currentMode = MODE_IDLE;
      break;
  }
}

void safetyShutdown() {
  heaterOn = false;
  fansOn = false;
  inCooldownMode = false;
  digitalWrite(HEATER_SSR_PIN, LOW);
  digitalWrite(FAN_RELAY_PIN, LOW);
  currentMode = MODE_IDLE;
  Serial.println(F("=== SAFETY SHUTDOWN ==="));
}

void updateDisplay() {
  // Make a thread-safe copy of display data
  float dispAvgTemp, dispHeaterTemp;
  bool dispHeaterOn, dispFansOn, dispHeaterDetected, dispInCooldown;
  float dispLeftTemps[3], dispRightTemps[3];
  int dispNumLeft, dispNumRight;
  float dispTempMin, dispTempMax;
  SystemMode dispMode;

  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    dispAvgTemp = averageTemp;
    dispHeaterTemp = heaterTemp;
    dispHeaterOn = heaterOn;
    dispFansOn = fansOn;
    dispHeaterDetected = heaterSensorDetected;
    dispInCooldown = inCooldownMode;
    dispNumLeft = numLeftSensors;
    dispNumRight = numRightSensors;
    dispTempMin = TEMP_MIN;
    dispTempMax = TEMP_MAX;
    dispMode = currentMode;
    for (int i = 0; i < 3; i++) {
      dispLeftTemps[i] = leftTemperatures[i];
      dispRightTemps[i] = rightTemperatures[i];
    }
    xSemaphoreGive(dataMutex);
  } else {
    // Skip this display update if we can't get the mutex
    return;
  }

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
  bool hasError = (dispHeaterTemp <= -100.0 || dispAvgTemp <= -100.0 || !dispHeaterDetected);
  if (hasError) {
    tft.fillRect(0, 20, 160, 20, ST77XX_BLACK);
    tft.setCursor(5, 20);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_RED);
    tft.println(F("SENSOR ERROR"));
  }

  // Average temperature
  if (abs(dispAvgTemp - lastDisplayedAvg) > 0.1 || !hasError) {
    tft.fillRect(0, 40, 160, 20, ST77XX_BLACK);
    tft.setCursor(1, 40);
    tft.setTextSize(2);

    if (dispAvgTemp > -100) {
      if (dispAvgTemp < dispTempMin) {
        tft.setTextColor(ST77XX_CYAN);
      } else if (dispAvgTemp > dispTempMax) {
        tft.setTextColor(ST77XX_ORANGE);
      } else {
        tft.setTextColor(ST77XX_YELLOW);
      }
      tft.print(dispAvgTemp, 1);
      tft.print(F("C"));
    } else {
      tft.setTextColor(ST77XX_RED);
      tft.print(F("ERROR"));
    }
    lastDisplayedAvg = dispAvgTemp;
  }

  // Heater status
  tft.fillRect(0, 65, 160, 12, ST77XX_BLACK);
  tft.setCursor(2, 65);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_RED);
  if (dispHeaterTemp > -100) {
    tft.print(F("H:"));
    tft.print(dispHeaterTemp, 1);
    tft.print(F("C "));
  } else {
    tft.print(F("H:ERR "));
  }
  tft.print(dispHeaterOn ? F("ON") : F("OFF"));

  // Fan status (show cooldown mode)
  tft.fillRect(0, 80, 160, 12, ST77XX_BLACK);
  tft.setCursor(2, 80);
  tft.setTextColor(ST77XX_WHITE);
  tft.print(F("Fan: "));
  tft.print(dispFansOn ? F("ON") : F("OFF"));
  if (dispInCooldown) {
    tft.setTextColor(ST77XX_YELLOW);
    tft.print(F(" [CD]"));
  }

  // Sensor values
  tft.fillRect(0, 95, 160, 33, ST77XX_BLACK);
  tft.setCursor(2, 95);
  tft.setTextColor(ST77XX_CYAN);

  for (int i = 0; i < dispNumLeft && i < 3; i++) {
    if (dispLeftTemps[i] > -100) {
      tft.print(dispLeftTemps[i], 1);
      tft.print(F(" "));
    } else {
      tft.print(F("-- "));
    }
  }

  tft.setCursor(2, 107);
  for (int i = 0; i < dispNumRight && i < 3; i++) {
    if (dispRightTemps[i] > -100) {
      tft.print(dispRightTemps[i], 1);
      tft.print(F(" "));
    } else {
      tft.print(F("-- "));
    }
  }
}

void handleRoot() {
  // Pre-allocate string to prevent fragmentation
  String html;
  html.reserve(3072);  // 3KB sufficient for current HTML size

  // Thread-safe copy of data
  float safeAvgTemp, safeHeaterTemp;
  float safeTempMin, safeTempMax, safeHeaterMax;
  bool safeHeaterOn, safeFansOn, safeHeaterDetected;
  float safeLeftTemps[3], safeRightTemps[3];
  int safeNumLeft, safeNumRight;

  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    safeAvgTemp = averageTemp;
    safeHeaterTemp = heaterTemp;
    safeTempMin = TEMP_MIN;
    safeTempMax = TEMP_MAX;
    safeHeaterMax = HEATER_SAFETY_MAX;
    safeHeaterOn = heaterOn;
    safeFansOn = fansOn;
    safeHeaterDetected = heaterSensorDetected;
    safeNumLeft = numLeftSensors;
    safeNumRight = numRightSensors;
    for (int i = 0; i < 3; i++) {
      safeLeftTemps[i] = leftTemperatures[i];
      safeRightTemps[i] = rightTemperatures[i];
    }
    xSemaphoreGive(dataMutex);
  } else {
    server.send(503, F("text/plain"), F("System busy"));
    return;
  }

  html = F("<!DOCTYPE html><html><head>"
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

  if (!safeHeaterDetected || safeHeaterTemp <= -100.0 || safeAvgTemp <= -100.0) {
    html += F("<div class='status' style='background:#f44336;'>FAULT</div><br>");
  }

  html += F("<div class='status ");
  html += safeHeaterOn ? F("on") : F("off");
  html += F("' id='heaterState'>Heater: ");
  html += safeHeaterOn ? F("ON") : F("OFF");
  html += F("</div>");

  html += F("<div class='status ");
  html += safeFansOn ? F("on") : F("off");
  html += F("' id='fansState'>Fans: ");
  html += safeFansOn ? F("ON") : F("OFF");
  html += F("</div></div>");

  html += F("<div class='card'><h2>Temperatures</h2>"
    "<div class='temp' id='avgTemp'>");
  html += String(safeAvgTemp, 1);
  html += F("C</div>"
    "<p style='text-align:center;color:#aaa;'>Average Temperature</p>"
    "<p style='text-align:center;'>Heater: <span id='heatTemp' style='color:#FF6B6B;font-weight:bold;'>");
  html += String(safeHeaterTemp, 1);
  html += F("C</span></p></div>");

  html += F("<div class='card'><h2>Sensors</h2><div class='sensor-grid'>");

  for (int i = 0; i < safeNumLeft && i < 3; i++) {
    html += F("<div class='sensor'><div style='color:#aaa;'>L");
    html += String(i);
    html += F("</div>");
    if (safeLeftTemps[i] > -100) {
      html += F("<div style='color:#87CEEB;font-weight:bold;'>");
      html += String(safeLeftTemps[i], 1);
      html += F("C</div>");
    } else {
      html += F("<div style='color:#f44336;'>ERR</div>");
    }
    html += F("</div>");
  }

  for (int i = 0; i < safeNumRight && i < 3; i++) {
    html += F("<div class='sensor'><div style='color:#aaa;'>R");
    html += String(i);
    html += F("</div>");
    if (safeRightTemps[i] > -100) {
      html += F("<div style='color:#87CEEB;font-weight:bold;'>");
      html += String(safeRightTemps[i], 1);
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
  html += String(safeTempMin, 1);
  html += F("C</span>"
    "<button class='btn' onclick='adjust(\"tempmin\",\"down\")'>-</button>"
    "<button class='btn' onclick='adjust(\"tempmin\",\"up\")'>+</button></div>"
    "<div class='control'><span>Cooling ON at:</span>"
    "<span class='value' id='tempMaxVal'>");
  html += String(safeTempMax, 1);
  html += F("C</span>"
    "<button class='btn' onclick='adjust(\"tempmax\",\"down\")'>-</button>"
    "<button class='btn' onclick='adjust(\"tempmax\",\"up\")'>+</button></div>"
    "<div class='control'><span>Heater Max:</span>"
    "<span class='value' id='heaterMaxVal'>");
  html += String(safeHeaterMax, 1);
  html += F("C</span>"
    "<button class='btn' onclick='adjust(\"heatmax\",\"down\")'>-</button>"
    "<button class='btn' onclick='adjust(\"heatmax\",\"up\")'>+</button></div></div>"
    "<div style='text-align:center;margin-top:30px;color:#666;font-size:12px;'>"
    "Greenhouse Controller v4.3</div></div></body></html>");
  
  server.send(200, F("text/html"), html);
}

void handleAdjust() {
  if (server.hasArg(F("param")) && server.hasArg(F("action"))) {
    String param = server.arg(F("param"));
    String action = server.arg(F("action"));
    float delta = (action == F("up")) ? 0.5 : -0.5;

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (param == F("tempmin")) {
        TEMP_MIN += delta;
        TEMP_MIN = constrain(TEMP_MIN, 5.0, 15.0);
        Serial.print(F("TEMP_MIN adjusted to: "));
        Serial.println(TEMP_MIN, 1);
      } else if (param == F("tempmax")) {
        TEMP_MAX += delta;
        TEMP_MAX = constrain(TEMP_MAX, 20.0, 35.0);
        Serial.print(F("TEMP_MAX adjusted to: "));
        Serial.println(TEMP_MAX, 1);
      } else if (param == F("heatmax")) {
        HEATER_SAFETY_MAX += delta;
        HEATER_SAFETY_MAX = constrain(HEATER_SAFETY_MAX, 30.0, 40.0);
        Serial.print(F("HEATER_SAFETY_MAX adjusted to: "));
        Serial.println(HEATER_SAFETY_MAX, 1);
      }
      xSemaphoreGive(dataMutex);
    }
  }

  server.sendHeader(F("Location"), F("/"));
  server.send(303);
}

void handleStatusJSON() {
  StaticJsonDocument<256> doc;

  // Thread-safe copy of data
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
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

    xSemaphoreGive(dataMutex);
  } else {
    server.send(503, F("application/json"), F("{\"error\":\"busy\"}"));
    return;
  }

  String output;
  serializeJson(doc, output);
  server.send(200, F("application/json"), output);
}