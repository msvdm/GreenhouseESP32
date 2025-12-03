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
#include <Preferences.h>

// ============================================================================
// GREENHOUSE CONTROLLER v5.0
// ============================================================================
// Safety-Critical System for 2200W Heating Element Control
// - Multiple independent safety layers
// - Persistent configuration storage
// - Runtime and cycle limits
// - Comprehensive error recovery
// ============================================================================

const char* FIRMWARE_VERSION = "5.0.0";

// WiFi Credentials (Access Point Mode)
const char* ap_ssid = "Green";
const char* ap_password = "xD8ro6rNdcxbMWy!P78H";

// ============================================================================
// PIN DEFINITIONS - THREE SEPARATE BUSES
// ============================================================================
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

// ============================================================================
// SAFETY LIMITS - CRITICAL PARAMETERS
// ============================================================================
// Temperature Thresholds (adjustable via web interface)
float TEMP_MIN = 10.0;              // Start heating below this
float TEMP_MAX = 25.0;              // Start cooling above this
float HEATER_SAFETY_MAX = 35.0;     // Heater air temperature limit
float HEATER_CRITICAL_MAX = 50.0;   // Fan temperature limit (AC outdoor unit rated)

#define HYSTERESIS 0.5              // Temperature deadband
#define MODE_CHANGE_HYSTERESIS 1.0  // Extra hysteresis to prevent mode oscillation
#define HEATING_TARGET_OFFSET 2.0   // Heat until TEMP_MIN + this offset (prevents short cycling)
#define MAX_TEMP_CHANGE_PER_CYCLE 5.0  // Max C change between readings (spike filter)

// ============================================================================
// SAFETY TIMERS - PREVENT RUNAWAY CONDITIONS
// ============================================================================
#define MAX_HEATER_RUNTIME 1800000      // 30 minutes max continuous heating
#define MIN_HEATER_OFF_TIME 300000      // 5 minutes minimum between heating cycles
#define FAN_STARTUP_DELAY 5000          // 5 seconds for fan to reach full airflow
#define FAN_COOLDOWN_TIME 60000         // 60 seconds fan runs after heater off
#define MAX_HEATING_CYCLES_PER_HOUR 6   // Prevent excessive cycling
#define SENSOR_FAILURE_GRACE_PERIOD 30000  // 30s grace before shutdown on sensor error

// ============================================================================
// TIMING INTERVALS
// ============================================================================
#define AIR_TEMP_READ_INTERVAL 5000
#define HEATER_TEMP_READ_INTERVAL 1000  // Critical - check frequently
#define DISPLAY_UPDATE_INTERVAL 2000
#define STATS_LOG_INTERVAL 300000       // Log statistics every 5 minutes
#define STATS_SAVE_INTERVAL 3600000     // Save statistics every hour (prevent data loss)
#define EEPROM_SAVE_DELAY 10000         // Save settings 10s after last change

// ============================================================================
// HARDWARE SETUP
// ============================================================================
OneWire leftSensorsBus(LEFT_SENSORS_BUS);
OneWire rightSensorsBus(RIGHT_SENSORS_BUS);
OneWire heaterSensorBus(HEATER_SENSOR_BUS);

DallasTemperature leftSensors(&leftSensorsBus);
DallasTemperature rightSensors(&rightSensorsBus);
DallasTemperature heaterSensor(&heaterSensorBus);

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
WebServer server(80);
Preferences preferences;

// ============================================================================
// SENSOR CONFIGURATION
// ============================================================================
DeviceAddress leftSensorAddresses[3];
DeviceAddress rightSensorAddresses[3];
DeviceAddress heaterSensorAddress;

int numLeftSensors = 0;
int numRightSensors = 0;
bool heaterSensorDetected = false;

// Temperature readings with validation
float leftTemperatures[3] = {-127.0, -127.0, -127.0};
float rightTemperatures[3] = {-127.0, -127.0, -127.0};
float prevLeftTemperatures[3] = {-127.0, -127.0, -127.0};
float prevRightTemperatures[3] = {-127.0, -127.0, -127.0};
float averageTemp = -127.0;
float heaterTemp = -127.0;
float prevHeaterTemp = -127.0;

// ============================================================================
// SYSTEM STATE TRACKING
// ============================================================================
enum SystemMode {
  MODE_IDLE,
  MODE_HEATING,
  MODE_COOLING,
  MODE_COOLDOWN,
  MODE_FAULT
};
SystemMode currentMode = MODE_IDLE;

bool heaterOn = false;
bool fansOn = false;
bool inCooldownMode = false;
bool systemFault = false;
String faultReason = "";

// ============================================================================
// SAFETY TRACKING VARIABLES
// ============================================================================
unsigned long heaterStartTime = 0;          // When current heating cycle started
unsigned long heaterTotalRuntime = 0;       // Total runtime this session
unsigned long lastHeaterOffTime = 0;        // When heater was last turned off
unsigned long fanStartTime = 0;             // When fans were turned on
unsigned long fanCooldownStart = 0;         // Cooldown timer
unsigned long lastSensorFailureTime = 0;    // Grace period for transient sensor errors
unsigned long settingsChangedTime = 0;      // Debounce EEPROM writes

// Cycle tracking (sliding window - last hour)
#define CYCLE_HISTORY_SIZE 10
unsigned long heatingCycleTimestamps[CYCLE_HISTORY_SIZE];
int cycleHistoryIndex = 0;

// ============================================================================
// THREAD SYNCHRONIZATION
// ============================================================================
SemaphoreHandle_t dataMutex;

// ============================================================================
// DISPLAY STATE
// ============================================================================
bool displayInitialized = false;
float lastDisplayedAvg = -999.0;
String webServerIP = "192.168.4.1";

// ============================================================================
// TIMING VARIABLES
// ============================================================================
unsigned long lastAirTempRead = 0;
unsigned long lastHeaterTempRead = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastStatsLog = 0;
unsigned long lastStatsSave = 0;

// ============================================================================
// STATISTICS
// ============================================================================
struct SystemStats {
  unsigned long totalHeatingCycles = 0;
  unsigned long totalCoolingCycles = 0;
  unsigned long totalHeaterRuntime = 0;
  float minTempRecorded = 999.0;
  float maxTempRecorded = -999.0;
  unsigned long lastResetTime = 0;
  unsigned long safetyShutdownCount = 0;
} stats;

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================
void readHeaterTemperature();
void readAirTemperatures();
void calculateAverage();
void controlSystem();
void safetyShutdown(String reason);
void updateDisplay();
void handleRoot();
void handleAdjust();
void handleStatusJSON();
void handleStats();
void handleReset();
void loadSettings();
void saveSettings();
bool checkHeatingCycleLimit();
void logStatistics();
void clearFault();

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println(F("\n╔════════════════════════════════════════╗"));
  Serial.println(F("║  GREENHOUSE CONTROLLER v5.0            ║"));
  Serial.println(F("║  Safety-Critical System                ║"));
  Serial.println(F("╚════════════════════════════════════════╝\n"));

  // Create mutex for thread-safe data access
  dataMutex = xSemaphoreCreateMutex();

  // Initialize outputs to SAFE state FIRST
  pinMode(HEATER_SSR_PIN, OUTPUT);
  pinMode(FAN_RELAY_PIN, OUTPUT);
  digitalWrite(HEATER_SSR_PIN, LOW);
  digitalWrite(FAN_RELAY_PIN, LOW);
  Serial.println(F("✓ Safety: All outputs OFF"));

  // Configure watchdog
  esp_task_wdt_init(20, true);  // 20 second timeout
  esp_task_wdt_add(NULL);
  Serial.println(F("✓ Watchdog configured (20s)"));

  // Load saved settings from EEPROM
  loadSettings();

  // Initialize timing to prevent rollover issues
  unsigned long now = millis();
  lastAirTempRead = now;
  lastHeaterTempRead = now;
  lastDisplayUpdate = now;
  lastStatsLog = now;
  lastStatsSave = now;
  stats.lastResetTime = now;
  
  // Initialize cycle history
  for (int i = 0; i < CYCLE_HISTORY_SIZE; i++) {
    heatingCycleTimestamps[i] = 0;
  }

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
      Serial.print(F("  ✓ L"));
      Serial.print(i);
      Serial.println(F(" initialized"));
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
      Serial.print(F("  ✓ R"));
      Serial.print(i);
      Serial.println(F(" initialized"));
    }
  }
  
  // Initialize HEATER sensor (CRITICAL)
  Serial.println(F("\n[Bus 3] HEATER sensor (GPIO 16)"));
  heaterSensor.begin();
  heaterSensor.setWaitForConversion(false);
  int heaterSensorCount = heaterSensor.getDeviceCount();
  Serial.print(F("  Found: "));
  Serial.println(heaterSensorCount);
  
  if (heaterSensorCount == 1 && heaterSensor.getAddress(heaterSensorAddress, 0)) {
    heaterSensor.setResolution(heaterSensorAddress, 12);
    heaterSensorDetected = true;
    Serial.println(F("  ✓ Heater sensor initialized"));
  } else {
    Serial.println(F("  ✗ ERROR: Heater sensor missing!"));
    Serial.println(F("  ⚠ HEATING DISABLED"));
    safetyShutdown("Heater sensor not detected");
  }
  
  // Initialize display
  Serial.println(F("\n[Display] Initializing TFT..."));
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(0, 0);
  tft.println(F("Greenhouse v5.0"));
  tft.print(F("Sensors: L"));
  tft.print(numLeftSensors);
  tft.print(F(" R"));
  tft.print(numRightSensors);
  tft.print(F(" H"));
  tft.println(heaterSensorDetected ? "1" : "0");
  
  // Setup WiFi Access Point
  Serial.println(F("\n[WiFi] Starting Access Point..."));
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  
  IPAddress IP = WiFi.softAPIP();
  webServerIP = IP.toString();
  Serial.print(F("  SSID: "));
  Serial.println(ap_ssid);
  Serial.print(F("  IP: "));
  Serial.println(IP);
  
  tft.print(F("IP: "));
  tft.println(IP);
  
  // Setup web server endpoints
  server.on("/", handleRoot);
  server.on("/adjust", handleAdjust);
  server.on("/status", handleStatusJSON);
  server.on("/stats", handleStats);
  server.on("/reset", handleReset);
  server.begin();
  Serial.println(F("✓ Web server started"));
  
  delay(2000);
  
  Serial.println(F("\n╔════════════════════════════════════════╗"));
  Serial.println(F("║  SYSTEM READY                          ║"));
  Serial.println(F("╚════════════════════════════════════════╝\n"));
  
  Serial.print(F("Temperature Range: "));
  Serial.print(TEMP_MIN, 1);
  Serial.print(F("C - "));
  Serial.print(TEMP_MAX, 1);
  Serial.println(F("C"));
  
  Serial.print(F("Heater Safety: "));
  Serial.print(HEATER_SAFETY_MAX, 1);
  Serial.print(F("C / Critical: "));
  Serial.print(HEATER_CRITICAL_MAX, 1);
  Serial.println(F("C\n"));
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
  unsigned long currentMillis = millis();

  // Handle web server requests (non-blocking)
  server.handleClient();

  // CRITICAL: Read heater temperature frequently
  if (currentMillis - lastHeaterTempRead >= HEATER_TEMP_READ_INTERVAL) {
    lastHeaterTempRead = currentMillis;

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      readHeaterTemperature();
      xSemaphoreGive(dataMutex);
    }

    esp_task_wdt_reset();
  }

  // Read air temperatures less frequently
  if (currentMillis - lastAirTempRead >= AIR_TEMP_READ_INTERVAL) {
    lastAirTempRead = currentMillis;

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      readAirTemperatures();
      calculateAverage();
      controlSystem();
      xSemaphoreGive(dataMutex);
    }

    esp_task_wdt_reset();
  }

  // Update display
  if (currentMillis - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
    lastDisplayUpdate = currentMillis;
    updateDisplay();
    esp_task_wdt_reset();
  }

  // Log statistics periodically
  if (currentMillis - lastStatsLog >= STATS_LOG_INTERVAL) {
    lastStatsLog = currentMillis;
    logStatistics();
  }

  // Save statistics periodically (prevent data loss on power failure)
  if (currentMillis - lastStatsSave >= STATS_SAVE_INTERVAL) {
    lastStatsSave = currentMillis;
    saveSettings();
    Serial.println(F("💾 Periodic stats save"));
  }

  // Save settings if changed (debounced)
  if (settingsChangedTime > 0 && (currentMillis - settingsChangedTime) >= EEPROM_SAVE_DELAY) {
    saveSettings();
    settingsChangedTime = 0;
  }

  delay(10);  // Prevent tight looping
}

// ============================================================================
// HEATER TEMPERATURE MONITORING - SAFETY CRITICAL
// ============================================================================
void readHeaterTemperature() {
  if (!heaterSensorDetected) {
    heaterTemp = -127.0;
    return;
  }
  
  heaterSensor.requestTemperatures();
  float temp = heaterSensor.getTempC(heaterSensorAddress);
  
  // Validate reading
  if (temp == DEVICE_DISCONNECTED_C || temp < -50 || temp > 100) {
    Serial.println(F("⚠ HEATER SENSOR READ ERROR"));
    
    // Grace period for transient errors
    if (lastSensorFailureTime == 0) {
      lastSensorFailureTime = millis();
      return;  // First error - give it time
    } else if (millis() - lastSensorFailureTime < SENSOR_FAILURE_GRACE_PERIOD) {
      return;  // Still within grace period
    }
    
    heaterTemp = -127.0;
    safetyShutdown("Heater sensor failure");
    return;
  }
  
  // Reset grace period on successful read
  lastSensorFailureTime = 0;
  
  // Rate of change validation (spike filter)
  if (prevHeaterTemp > -100) {
    float change = abs(temp - prevHeaterTemp);
    if (change > MAX_TEMP_CHANGE_PER_CYCLE) {
      Serial.print(F("⚠ Heater sensor spike rejected: "));
      Serial.print(change, 1);
      Serial.println(F("C"));
      return;  // Keep previous reading
    }
  }
  
  prevHeaterTemp = heaterTemp;
  heaterTemp = temp;
  
  // CRITICAL SAFETY CHECK - Immediate shutdown
  if (heaterTemp >= HEATER_CRITICAL_MAX) {
    Serial.print(F("🚨 CRITICAL: Heater temp "));
    Serial.print(heaterTemp, 1);
    Serial.println(F("C - FAN LIMIT EXCEEDED"));
    safetyShutdown("Heater critical temperature");
  } else if (heaterTemp >= HEATER_SAFETY_MAX) {
    Serial.print(F("⚠ WARNING: Heater temp "));
    Serial.print(heaterTemp, 1);
    Serial.println(F("C - Safety limit reached"));
    if (heaterOn) {
      safetyShutdown("Heater safety temperature exceeded");
    }
  }
}

// ============================================================================
// AIR TEMPERATURE MONITORING
// ============================================================================
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
          continue;  // Skip spike
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
          continue;  // Skip spike
        }
      }
      prevRightTemperatures[i] = rightTemperatures[i];
      rightTemperatures[i] = temp;
    } else {
      rightTemperatures[i] = -127.0;
    }
  }
}

// ============================================================================
// CALCULATE AVERAGE TEMPERATURE
// ============================================================================
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
    
    // Update statistics
    if (averageTemp < stats.minTempRecorded) stats.minTempRecorded = averageTemp;
    if (averageTemp > stats.maxTempRecorded) stats.maxTempRecorded = averageTemp;
  } else {
    Serial.println(F("⚠ ERROR: No valid air sensors"));
    averageTemp = -127.0;
    
    // Grace period before shutdown
    if (lastSensorFailureTime == 0) {
      lastSensorFailureTime = millis();
    } else if (millis() - lastSensorFailureTime >= SENSOR_FAILURE_GRACE_PERIOD) {
      safetyShutdown("All air sensors failed");
    }
    return;
  }
  
  // Reset grace period on successful read
  lastSensorFailureTime = 0;
}

// ============================================================================
// MAIN CONTROL SYSTEM - WITH COMPREHENSIVE SAFETY CHECKS
// ============================================================================
void controlSystem() {
  unsigned long currentMillis = millis();

  // SAFETY: Cannot operate without valid sensors
  if (averageTemp <= -100.0 || heaterTemp <= -100.0 || !heaterSensorDetected) {
    if (heaterOn || (fansOn && !inCooldownMode)) {
      safetyShutdown("Invalid sensor readings");
    }
    return;
  }

  // SAFETY: Check if system is in fault state
  if (systemFault) {
    // Keep everything off
    if (heaterOn || fansOn) {
      digitalWrite(HEATER_SSR_PIN, LOW);
      digitalWrite(FAN_RELAY_PIN, LOW);
      heaterOn = false;
      fansOn = false;
    }
    return;
  }

  // SAFETY: Check maximum heater runtime
  if (heaterOn && (currentMillis - heaterStartTime >= MAX_HEATER_RUNTIME)) {
    Serial.println(F("⚠ Max heater runtime reached - Forcing cooldown"));
    heaterOn = false;
    digitalWrite(HEATER_SSR_PIN, LOW);
    inCooldownMode = true;
    fanCooldownStart = currentMillis;
    currentMode = MODE_COOLDOWN;
    stats.totalHeaterRuntime += (currentMillis - heaterStartTime);
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
      Serial.println(F("✓ Cooldown complete - System IDLE"));
    }

    // Emergency: If temp gets too high during cooldown, enter cooling mode
    if (averageTemp > TEMP_MAX + MODE_CHANGE_HYSTERESIS) {
      inCooldownMode = false;
      currentMode = MODE_COOLING;
      Serial.println(F("⚠ Cooldown aborted - Switching to COOLING"));
    }

    return;
  }

  // Determine target mode based on temperature with hysteresis
  SystemMode targetMode = MODE_IDLE;

  if (averageTemp < TEMP_MIN - (currentMode == MODE_HEATING ? 0 : MODE_CHANGE_HYSTERESIS)) {
    targetMode = MODE_HEATING;
  } else if (averageTemp > TEMP_MAX + (currentMode == MODE_COOLING ? 0 : MODE_CHANGE_HYSTERESIS)) {
    targetMode = MODE_COOLING;
  } else if (averageTemp >= TEMP_MIN + HEATING_TARGET_OFFSET + HYSTERESIS && currentMode == MODE_HEATING) {
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
    switch(currentMode) {
      case MODE_IDLE: Serial.print(F("IDLE")); break;
      case MODE_HEATING: Serial.print(F("HEATING")); break;
      case MODE_COOLING: Serial.print(F("COOLING")); break;
      case MODE_COOLDOWN: Serial.print(F("COOLDOWN")); break;
      case MODE_FAULT: Serial.print(F("FAULT")); break;
    }
    Serial.print(F(" → "));
    switch(targetMode) {
      case MODE_IDLE: Serial.println(F("IDLE")); break;
      case MODE_HEATING: Serial.println(F("HEATING")); break;
      case MODE_COOLING: Serial.println(F("COOLING")); break;
      case MODE_COOLDOWN: Serial.println(F("COOLDOWN")); break;
      case MODE_FAULT: Serial.println(F("FAULT")); break;
    }
  }

  // Execute control logic based on target mode
  switch (targetMode) {
    case MODE_HEATING:
      // SAFETY: Check minimum off time between heating cycles
      if (!heaterOn && lastHeaterOffTime > 0) {
        if (currentMillis - lastHeaterOffTime < MIN_HEATER_OFF_TIME) {
          Serial.print(F("⏳ Minimum off-time: "));
          Serial.print((MIN_HEATER_OFF_TIME - (currentMillis - lastHeaterOffTime)) / 1000);
          Serial.println(F("s remaining"));
          return;  // Wait for minimum off time
        }
      }

      // SAFETY: Check heating cycle limit
      if (!heaterOn && !checkHeatingCycleLimit()) {
        Serial.println(F("⚠ Heating cycle limit reached - Waiting..."));
        return;
      }

      // CRITICAL: Fans MUST be on before heater
      if (!fansOn) {
        fansOn = true;
        fanStartTime = currentMillis;
        digitalWrite(FAN_RELAY_PIN, HIGH);
        Serial.println(F("Fans ON (heating prep)"));
        return;  // Exit this cycle - wait for fans to spin up
      }

      // SAFETY: Wait for fans to reach full speed
      if (currentMillis - fanStartTime < FAN_STARTUP_DELAY) {
        Serial.print(F("⏳ Fan startup: "));
        Serial.print((FAN_STARTUP_DELAY - (currentMillis - fanStartTime)) / 1000);
        Serial.println(F("s remaining"));
        return;
      }

      // CRITICAL: Only turn on heater if ALL conditions are safe
      if (!heaterOn &&
          heaterTemp > 0 &&
          heaterTemp < HEATER_SAFETY_MAX - 2.0 &&  // 2C safety margin
          fansOn &&
          (currentMillis - fanStartTime >= FAN_STARTUP_DELAY)) {
        
        heaterOn = true;
        heaterStartTime = currentMillis;
        digitalWrite(HEATER_SSR_PIN, HIGH);
        
        // Record cycle start
        heatingCycleTimestamps[cycleHistoryIndex] = currentMillis;
        cycleHistoryIndex = (cycleHistoryIndex + 1) % CYCLE_HISTORY_SIZE;
        stats.totalHeatingCycles++;
        
        Serial.print(F("🔥 Heater ON: Avg="));
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
        lastHeaterOffTime = currentMillis;
        stats.totalHeaterRuntime += (currentMillis - heaterStartTime);
        Serial.println(F("Heater OFF (cooling mode)"));
      }

      // Turn on fans for cooling
      if (!fansOn) {
        fansOn = true;
        digitalWrite(FAN_RELAY_PIN, HIGH);
        stats.totalCoolingCycles++;
        Serial.print(F("❄️  COOLING: "));
        Serial.print(averageTemp, 1);
        Serial.println(F("C"));
      }
      currentMode = MODE_COOLING;
      break;

    case MODE_COOLDOWN:
      // Enter cooldown mode - heater off, fans stay on
      if (heaterOn) {
        heaterOn = false;
        digitalWrite(HEATER_SSR_PIN, LOW);
        lastHeaterOffTime = currentMillis;
        stats.totalHeaterRuntime += (currentMillis - heaterStartTime);
        Serial.println(F("Heater OFF - Starting cooldown"));
      }

      inCooldownMode = true;
      fanCooldownStart = currentMillis;
      currentMode = MODE_COOLDOWN;
      Serial.print(F("⏳ COOLDOWN started ("));
      Serial.print(FAN_COOLDOWN_TIME / 1000);
      Serial.println(F("s)"));
      break;

    case MODE_IDLE:
      // Turn everything off
      if (heaterOn) {
        heaterOn = false;
        digitalWrite(HEATER_SSR_PIN, LOW);
        lastHeaterOffTime = currentMillis;
        stats.totalHeaterRuntime += (currentMillis - heaterStartTime);
        Serial.println(F("Heater OFF (idle)"));
      }
      if (fansOn) {
        fansOn = false;
        digitalWrite(FAN_RELAY_PIN, LOW);
        Serial.println(F("Fans OFF (idle)"));
      }
      if (currentMode != MODE_IDLE) {
        Serial.print(F("💤 IDLE: "));
        Serial.print(averageTemp, 1);
        Serial.println(F("C"));
      }
      currentMode = MODE_IDLE;
      break;

    case MODE_FAULT:
      // Handled at top of function
      break;
  }
}

// ============================================================================
// CHECK HEATING CYCLE LIMIT (Prevent excessive cycling)
// ============================================================================
bool checkHeatingCycleLimit() {
  unsigned long currentTime = millis();
  unsigned long oneHourAgo = currentTime - 3600000;  // 1 hour in ms
  
  int cyclesInLastHour = 0;
  for (int i = 0; i < CYCLE_HISTORY_SIZE; i++) {
    if (heatingCycleTimestamps[i] > oneHourAgo && heatingCycleTimestamps[i] > 0) {
      cyclesInLastHour++;
    }
  }
  
  return cyclesInLastHour < MAX_HEATING_CYCLES_PER_HOUR;
}

// ============================================================================
// SAFETY SHUTDOWN - IMMEDIATE EMERGENCY STOP
// ============================================================================
void safetyShutdown(String reason) {
  heaterOn = false;
  fansOn = false;
  inCooldownMode = false;
  systemFault = true;
  faultReason = reason;
  
  digitalWrite(HEATER_SSR_PIN, LOW);
  digitalWrite(FAN_RELAY_PIN, LOW);
  
  currentMode = MODE_FAULT;
  stats.safetyShutdownCount++;
  
  Serial.println(F("\n╔════════════════════════════════════════╗"));
  Serial.println(F("║     🚨 SAFETY SHUTDOWN 🚨              ║"));
  Serial.println(F("╚════════════════════════════════════════╝"));
  Serial.print(F("Reason: "));
  Serial.println(reason);
  Serial.println();
}

// ============================================================================
// CLEAR FAULT STATE (Manual intervention required)
// ============================================================================
void clearFault() {
  if (systemFault) {
    systemFault = false;
    faultReason = "";
    currentMode = MODE_IDLE;
    Serial.println(F("✓ Fault cleared - System resuming normal operation"));
  }
}

// ============================================================================
// UPDATE DISPLAY
// ============================================================================
void updateDisplay() {
  // Make thread-safe copy of display data
  float dispAvgTemp, dispHeaterTemp;
  bool dispHeaterOn, dispFansOn, dispHeaterDetected, dispInCooldown;
  bool dispFault;
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
    dispFault = systemFault;
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
    return;  // Skip if can't get mutex
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

  // Fault indicator (highest priority)
  if (dispFault) {
    tft.fillRect(0, 20, 160, 20, ST77XX_BLACK);
    tft.setCursor(5, 20);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_RED);
    tft.println(F("SYSTEM FAULT"));
    tft.setCursor(5, 30);
    tft.setTextColor(ST77XX_YELLOW);
    tft.println(F("Web: /reset"));
  } else {
    // Error indicator
    bool hasError = (dispHeaterTemp <= -100.0 || dispAvgTemp <= -100.0 || !dispHeaterDetected);
    if (hasError) {
      tft.fillRect(0, 20, 160, 20, ST77XX_BLACK);
      tft.setCursor(5, 20);
      tft.setTextSize(1);
      tft.setTextColor(ST77XX_ORANGE);
      tft.println(F("SENSOR ERROR"));
    } else {
      tft.fillRect(0, 20, 160, 20, ST77XX_BLACK);
    }
  }

  // Average temperature
  if (abs(dispAvgTemp - lastDisplayedAvg) > 0.1 || dispFault) {
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

  // Fan status
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
  
  // Runtime indicator (bottom line)
  tft.setCursor(2, 120);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_GREEN);
  unsigned long uptime = millis() / 1000;
  tft.print(F("Up:"));
  tft.print(uptime / 3600);
  tft.print(F("h"));
}

// ============================================================================
// WEB SERVER - ROOT PAGE
// ============================================================================
void handleRoot() {
  String html;
  html.reserve(4096);

  // Thread-safe copy of data
  float safeAvgTemp, safeHeaterTemp;
  float safeTempMin, safeTempMax, safeHeaterMax, safeHeaterCritical;
  bool safeHeaterOn, safeFansOn, safeHeaterDetected, safeFault;
  String safeFaultReason;
  float safeLeftTemps[3], safeRightTemps[3];
  int safeNumLeft, safeNumRight;
  unsigned long safeUptime, safeTotalCycles, safeTotalRuntime;

  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    safeAvgTemp = averageTemp;
    safeHeaterTemp = heaterTemp;
    safeTempMin = TEMP_MIN;
    safeTempMax = TEMP_MAX;
    safeHeaterMax = HEATER_SAFETY_MAX;
    safeHeaterCritical = HEATER_CRITICAL_MAX;
    safeHeaterOn = heaterOn;
    safeFansOn = fansOn;
    safeHeaterDetected = heaterSensorDetected;
    safeFault = systemFault;
    safeFaultReason = faultReason;
    safeNumLeft = numLeftSensors;
    safeNumRight = numRightSensors;
    safeUptime = millis() / 1000;
    safeTotalCycles = stats.totalHeatingCycles;
    safeTotalRuntime = stats.totalHeaterRuntime / 1000;
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
    "<title>Greenhouse Control v5.0</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;margin:0;padding:20px;background:#1a1a1a;color:#fff;}"
    ".container{max-width:800px;margin:0 auto;}"
    "h1{color:#4CAF50;text-align:center;margin-bottom:5px;}"
    ".version{text-align:center;color:#888;font-size:12px;margin-bottom:20px;}"
    ".card{background:#2a2a2a;padding:15px;margin:10px 0;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.3);}"
    ".temp{font-size:48px;font-weight:bold;color:#FFA500;text-align:center;margin:10px 0;}"
    ".status{display:inline-block;padding:10px 20px;border-radius:20px;margin:5px;font-weight:bold;font-size:14px;}"
    ".status.on{background:#4CAF50;color:#000;}"
    ".status.off{background:#666;color:#ccc;}"
    ".status.fault{background:#f44336;color:#fff;animation:blink 1s infinite;}"
    "@keyframes blink{50%{opacity:0.5;}}"
    ".control{display:flex;justify-content:space-between;align-items:center;margin:10px 0;padding:10px;background:#333;border-radius:5px;}"
    ".btn{background:#4CAF50;border:none;color:#000;padding:10px 20px;font-size:16px;font-weight:bold;border-radius:5px;cursor:pointer;margin:0 2px;}"
    ".btn:hover{background:#45a049;}"
    ".btn:active{background:#3d8b40;}"
    ".btn.danger{background:#f44336;color:#fff;}"
    ".btn.danger:hover{background:#da190b;}"
    ".value{font-size:18px;font-weight:bold;color:#4CAF50;}"
    ".sensor-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(80px,1fr));gap:10px;text-align:center;}"
    ".sensor{background:#333;padding:10px;border-radius:5px;}"
    ".sensor-label{color:#aaa;font-size:12px;}"
    ".sensor-value{color:#87CEEB;font-weight:bold;font-size:16px;margin-top:5px;}"
    ".info-row{display:flex;justify-content:space-between;margin:5px 0;}"
    ".info-label{color:#aaa;}"
    ".info-value{color:#fff;font-weight:bold;}"
    ".alert{background:#f44336;color:#fff;padding:15px;border-radius:8px;text-align:center;font-weight:bold;margin:10px 0;}"
    "h2{color:#4CAF50;border-bottom:2px solid #4CAF50;padding-bottom:5px;margin-top:20px;}"
    "</style>"
    "<script>"
    "async function update(){"
    "try{"
    "const r=await fetch('/status');"
    "const d=await r.json();"
    "document.getElementById('avgTemp').textContent=(d.avg>-100)?d.avg.toFixed(1)+'C':'ERROR';"
    "document.getElementById('heaterState').textContent=(d.heater==1)?'ON':'OFF';"
    "document.getElementById('heaterState').className='status '+((d.heater==1)?'on':'off');"
    "document.getElementById('fansState').textContent=(d.fans==1)?'ON':'OFF';"
    "document.getElementById('fansState').className='status '+((d.fans==1)?'on':'off');"
    "document.getElementById('heatTemp').textContent=(d.heat_temp>-100)?d.heat_temp.toFixed(1)+'C':'ERROR';"
    "document.getElementById('tempMinVal').textContent=d.temp_min.toFixed(1)+'C';"
    "document.getElementById('tempMaxVal').textContent=d.temp_max.toFixed(1)+'C';"
    "document.getElementById('heaterMaxVal').textContent=d.heater_max.toFixed(1)+'C';"
    "const faultDiv=document.getElementById('faultAlert');"
    "if(d.fault){faultDiv.style.display='block';document.getElementById('faultReason').textContent=d.fault_reason;}else{faultDiv.style.display='none';}"
    "}catch(e){console.error(e);}}"
    "async function adjust(p,a){"
    "await fetch('/adjust?param='+p+'&action='+a);"
    "setTimeout(update,200);}"
    "async function resetFault(){"
    "if(confirm('Clear system fault and resume operation?')){"
    "await fetch('/reset');"
    "setTimeout(()=>location.reload(),500);}}"
    "setInterval(update,2000);"
    "window.onload=update;"
    "</script>"
    "</head><body>"
    "<div class='container'>"
    "<h1>Greenhouse Controller</h1>"
    "<div class='version'>v");
  
  html += FIRMWARE_VERSION;
  html += F(" | Safety-Critical System</div>");

  // Fault alert
  if (safeFault) {
    html += F("<div id='faultAlert' class='alert'>"
      "SYSTEM FAULT<br>"
      "<span id='faultReason'>");
    html += safeFaultReason;
    html += F("</span><br>"
      "<button class='btn danger' onclick='resetFault()' style='margin-top:10px;'>Clear Fault & Resume</button>"
      "</div>");
  } else {
    html += F("<div id='faultAlert' class='alert' style='display:none;'>"
      "SYSTEM FAULT<br>"
      "<span id='faultReason'></span><br>"
      "<button class='btn danger' onclick='resetFault()' style='margin-top:10px;'>Clear Fault & Resume</button>"
      "</div>");
  }

  html += F("<div class='card'><h2>System Status</h2>");

  if (!safeHeaterDetected || safeHeaterTemp <= -100.0 || safeAvgTemp <= -100.0) {
    html += F("<div class='status fault'>⚠ SENSOR FAULT</div>");
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
  html += F("</div>");
  
  html += F("<div class='info-row'><span class='info-label'>Uptime:</span><span class='info-value'>");
  html += String(safeUptime / 3600);
  html += F("h ");
  html += String((safeUptime % 3600) / 60);
  html += F("m</span></div>");
  
  html += F("<div class='info-row'><span class='info-label'>Total Heating Cycles:</span><span class='info-value'>");
  html += String(safeTotalCycles);
  html += F("</span></div>");
  
  html += F("<div class='info-row'><span class='info-label'>Total Heater Runtime:</span><span class='info-value'>");
  html += String(safeTotalRuntime / 3600);
  html += F("h ");
  html += String((safeTotalRuntime % 3600) / 60);
  html += F("m</span></div>");
  
  html += F("</div>");

  html += F("<div class='card'><h2>Temperatures</h2>"
    "<div class='temp' id='avgTemp'>");
  html += String(safeAvgTemp, 1);
  html += F("C</div>"
    "<p style='text-align:center;color:#aaa;font-size:14px;'>Average Air Temperature</p>"
    "<p style='text-align:center;margin-top:15px;'>Heater Zone: <span id='heatTemp' style='color:#FF6B6B;font-weight:bold;font-size:20px;'>");
  html += String(safeHeaterTemp, 1);
  html += F("C</span></p></div>");

  html += F("<div class='card'><h2>Sensor Array</h2><div class='sensor-grid'>");

  for (int i = 0; i < safeNumLeft && i < 3; i++) {
    html += F("<div class='sensor'><div class='sensor-label'>LEFT ");
    html += String(i);
    html += F("</div>");
    if (safeLeftTemps[i] > -100) {
      html += F("<div class='sensor-value'>");
      html += String(safeLeftTemps[i], 1);
      html += F("C</div>");
    } else {
      html += F("<div class='sensor-value' style='color:#f44336;'>ERROR</div>");
    }
    html += F("</div>");
  }

  for (int i = 0; i < safeNumRight && i < 3; i++) {
    html += F("<div class='sensor'><div class='sensor-label'>RIGHT ");
    html += String(i);
    html += F("</div>");
    if (safeRightTemps[i] > -100) {
      html += F("<div class='sensor-value'>");
      html += String(safeRightTemps[i], 1);
      html += F("C</div>");
    } else {
      html += F("<div class='sensor-value' style='color:#f44336;'>ERROR</div>");
    }
    html += F("</div>");
  }

  html += F("</div></div>");

  html += F("<div class='card'><h2>Temperature Control</h2>"
    "<div class='control'><span>Heating Starts:</span>"
    "<span class='value' id='tempMinVal'>");
  html += String(safeTempMin, 1);
  html += F("C</span>"
    "<div><button class='btn' onclick='adjust(\"tempmin\",\"down\")'>-</button>"
    "<button class='btn' onclick='adjust(\"tempmin\",\"up\")'>+</button></div></div>"
    "<div class='control'><span>Cooling Starts:</span>"
    "<span class='value' id='tempMaxVal'>");
  html += String(safeTempMax, 1);
  html += F("C</span>"
    "<div><button class='btn' onclick='adjust(\"tempmax\",\"down\")'>-</button>"
    "<button class='btn' onclick='adjust(\"tempmax\",\"up\")'>+</button></div></div></div>");

  html += F("<div class='card'><h2>Safety Limits</h2>"
    "<div class='control'><span>Heater Safety Limit:</span>"
    "<span class='value' id='heaterMaxVal'>");
  html += String(safeHeaterMax, 1);
  html += F("C</span>"
    "<div><button class='btn' onclick='adjust(\"heatmax\",\"down\")'>-</button>"
    "<button class='btn' onclick='adjust(\"heatmax\",\"up\")'>+</button></div></div>"
    "<div class='info-row'><span class='info-label'>Critical Shutdown:</span><span class='info-value'>");
  html += String(safeHeaterCritical, 1);
  html += F("C</span></div>"
    "<p style='color:#888;font-size:12px;margin:10px 0 0 0;'>Max continuous runtime: 30 min | Min off-time: 5 min | Max cycles/hour: 6</p></div>");

  html += F("<div class='card'><h2>Quick Links</h2>"
    "<div style='text-align:center;'>"
    "<button class='btn' onclick='location.href=\"/stats\"'>Statistics</button>"
    "<button class='btn' onclick='location.reload()'>Refresh</button>"
    "</div></div>");

  html += F("<div style='text-align:center;margin-top:20px;color:#666;font-size:12px;'>"
    "Greenhouse Controller v");
  html += FIRMWARE_VERSION;
  html += F(" | © 2024<br>"
    "Hardware: ESP32 | 2200W Heater | DS18B20 Sensors | 220V AC Fan"
    "</div></div></body></html>");
  
  server.send(200, F("text/html"), html);
}

// ============================================================================
// WEB SERVER - ADJUST PARAMETERS
// ============================================================================
void handleAdjust() {
  if (server.hasArg(F("param")) && server.hasArg(F("action"))) {
    String param = server.arg(F("param"));
    String action = server.arg(F("action"));
    float delta = (action == F("up")) ? 0.5 : -0.5;

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (param == F("tempmin")) {
        TEMP_MIN += delta;
        TEMP_MIN = constrain(TEMP_MIN, 5.0, 20.0);
        settingsChangedTime = millis();
        Serial.print(F("TEMP_MIN → "));
        Serial.print(TEMP_MIN, 1);
        Serial.println(F("C"));
      } else if (param == F("tempmax")) {
        TEMP_MAX += delta;
        TEMP_MAX = constrain(TEMP_MAX, 15.0, 40.0);
        settingsChangedTime = millis();
        Serial.print(F("TEMP_MAX → "));
        Serial.print(TEMP_MAX, 1);
        Serial.println(F("C"));
      } else if (param == F("heatmax")) {
        HEATER_SAFETY_MAX += delta;
        HEATER_SAFETY_MAX = constrain(HEATER_SAFETY_MAX, 25.0, 45.0);
        settingsChangedTime = millis();
        Serial.print(F("HEATER_SAFETY_MAX → "));
        Serial.print(HEATER_SAFETY_MAX, 1);
        Serial.println(F("C"));
      }
      xSemaphoreGive(dataMutex);
    }
  }

  server.sendHeader(F("Location"), F("/"));
  server.send(303);
}

// ============================================================================
// WEB SERVER - STATUS JSON
// ============================================================================
void handleStatusJSON() {
  StaticJsonDocument<384> doc;

  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    doc["avg"] = (averageTemp > -100) ? averageTemp : -127.0;
    doc["heater"] = heaterOn ? 1 : 0;
    doc["fans"] = fansOn ? 1 : 0;
    doc["heat_temp"] = (heaterTemp > -100) ? heaterTemp : -127.0;
    doc["temp_min"] = TEMP_MIN;
    doc["temp_max"] = TEMP_MAX;
    doc["heater_max"] = HEATER_SAFETY_MAX;
    doc["fault"] = systemFault;
    doc["fault_reason"] = faultReason;
    doc["uptime"] = millis() / 1000;

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

// ============================================================================
// WEB SERVER - STATISTICS PAGE
// ============================================================================
void handleStats() {
  String html;
  html.reserve(2048);

  html = F("<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Statistics</title>"
    "<style>"
    "body{font-family:Arial;margin:20px;background:#1a1a1a;color:#fff;}"
    ".container{max-width:600px;margin:0 auto;}"
    "h1{color:#4CAF50;text-align:center;}"
    ".card{background:#2a2a2a;padding:15px;margin:10px 0;border-radius:8px;}"
    ".stat{display:flex;justify-content:space-between;margin:10px 0;padding:10px;background:#333;border-radius:5px;}"
    ".label{color:#aaa;}"
    ".value{color:#4CAF50;font-weight:bold;}"
    ".btn{background:#4CAF50;border:none;color:#000;padding:10px 20px;font-size:16px;font-weight:bold;border-radius:5px;cursor:pointer;width:100%;margin-top:10px;}"
    "</style>"
    "</head><body>"
    "<div class='container'>"
    "<h1>System Statistics</h1>"
    "<div class='card'>");

  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    html += F("<div class='stat'><span class='label'>Total Heating Cycles:</span><span class='value'>");
    html += String(stats.totalHeatingCycles);
    html += F("</span></div>");

    html += F("<div class='stat'><span class='label'>Total Cooling Cycles:</span><span class='value'>");
    html += String(stats.totalCoolingCycles);
    html += F("</span></div>");

    html += F("<div class='stat'><span class='label'>Total Heater Runtime:</span><span class='value'>");
    unsigned long totalHours = stats.totalHeaterRuntime / 3600000;
    unsigned long totalMinutes = (stats.totalHeaterRuntime % 3600000) / 60000;
    html += String(totalHours);
    html += F("h ");
    html += String(totalMinutes);
    html += F("m</span></div>");

    html += F("<div class='stat'><span class='label'>Min Temperature:</span><span class='value'>");
    html += String(stats.minTempRecorded, 1);
    html += F("C</span></div>");

    html += F("<div class='stat'><span class='label'>Max Temperature:</span><span class='value'>");
    html += String(stats.maxTempRecorded, 1);
    html += F("C</span></div>");

    html += F("<div class='stat'><span class='label'>Safety Shutdowns:</span><span class='value'>");
    html += String(stats.safetyShutdownCount);
    html += F("</span></div>");

    html += F("<div class='stat'><span class='label'>System Uptime:</span><span class='value'>");
    unsigned long uptime = (millis() - stats.lastResetTime) / 1000;
    html += String(uptime / 3600);
    html += F("h ");
    html += String((uptime % 3600) / 60);
    html += F("m</span></div>");

    xSemaphoreGive(dataMutex);
  }

  html += F("</div>"
    "<button class='btn' onclick='location.href=\"/\"'>Back to Control</button>"
    "</div></body></html>");

  server.send(200, F("text/html"), html);
}

// ============================================================================
// WEB SERVER - RESET FAULT
// ============================================================================
void handleReset() {
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    clearFault();
    xSemaphoreGive(dataMutex);
  }

  server.sendHeader(F("Location"), F("/"));
  server.send(303);
}

// ============================================================================
// LOAD SETTINGS FROM EEPROM
// ============================================================================
void loadSettings() {
  preferences.begin("greenhouse", false);
  
  TEMP_MIN = preferences.getFloat("temp_min", 10.0);
  TEMP_MAX = preferences.getFloat("temp_max", 25.0);
  HEATER_SAFETY_MAX = preferences.getFloat("heater_max", 35.0);
  
  stats.totalHeatingCycles = preferences.getULong("heat_cycles", 0);
  stats.totalCoolingCycles = preferences.getULong("cool_cycles", 0);
  stats.totalHeaterRuntime = preferences.getULong("heat_runtime", 0);
  stats.safetyShutdownCount = preferences.getULong("shutdowns", 0);
  
  preferences.end();
  
  Serial.println(F("✓ Settings loaded from EEPROM"));
}

// ============================================================================
// SAVE SETTINGS TO EEPROM
// ============================================================================
void saveSettings() {
  preferences.begin("greenhouse", false);
  
  preferences.putFloat("temp_min", TEMP_MIN);
  preferences.putFloat("temp_max", TEMP_MAX);
  preferences.putFloat("heater_max", HEATER_SAFETY_MAX);
  
  preferences.putULong("heat_cycles", stats.totalHeatingCycles);
  preferences.putULong("cool_cycles", stats.totalCoolingCycles);
  preferences.putULong("heat_runtime", stats.totalHeaterRuntime);
  preferences.putULong("shutdowns", stats.safetyShutdownCount);
  
  preferences.end();
  
  Serial.println(F("Settings saved to EEPROM"));
}

// ============================================================================
// LOG STATISTICS (Periodic)
// ============================================================================
void logStatistics() {
  Serial.println(F("\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
  Serial.println(F("           SYSTEM STATISTICS"));
  Serial.println(F("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
  
  Serial.print(F("Average Temp: "));
  Serial.print(averageTemp, 1);
  Serial.println(F("C"));
  
  Serial.print(F("Heater Zone: "));
  Serial.print(heaterTemp, 1);
  Serial.println(F("C"));
  
  Serial.print(F("Mode: "));
  switch(currentMode) {
    case MODE_IDLE: Serial.println(F("IDLE")); break;
    case MODE_HEATING: Serial.println(F("HEATING")); break;
    case MODE_COOLING: Serial.println(F("COOLING")); break;
    case MODE_COOLDOWN: Serial.println(F("COOLDOWN")); break;
    case MODE_FAULT: Serial.println(F("FAULT")); break;
  }
  
  Serial.print(F("Total Heating Cycles: "));
  Serial.println(stats.totalHeatingCycles);
  
  Serial.print(F("Total Runtime: "));
  Serial.print(stats.totalHeaterRuntime / 3600000);
  Serial.println(F("h"));
  
  Serial.print(F("Temp Range: "));
  Serial.print(stats.minTempRecorded, 1);
  Serial.print(F("C - "));
  Serial.print(stats.maxTempRecorded, 1);
  Serial.println(F("C"));
  
  Serial.println(F("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"));
}