// ============================================================================
// GREENHOUSE CONTROLLER
// ============================================================================
// Safety-critical control of a 2200W heating element and a circulation fan.
//
// This file is wiring only: bring the subsystems up in a safe order, then
// dispatch periodic work. All policy lives in the modules.
//
//   config.h   pins, limits, timings, persistent settings and statistics
//   sensors.h  DS18B20 acquisition and validation
//   control.h  relays, fault policy, mode machine   <- the only relay writer
//   display.h  TFT status screen
//   webui.h    static pages + /status JSON
//   ota.h      firmware update (ArduinoOTA and browser upload)
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "control.h"
#include "display.h"
#include "ota.h"
#include "sensors.h"
#include "webui.h"
#include "secrets.h"

// Periodic task bookkeeping. Each entry is "last time this ran".
static unsigned long lastHeaterRead = 0;
static unsigned long lastAirRead = 0;
static unsigned long lastDisplay = 0;
static unsigned long lastStatsLog = 0;
static unsigned long lastStatsSave = 0;

// ----------------------------------------------------------------------------
static void startAccessPoint() {
  Serial.println(F("\n[WiFi] Starting access point"));
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  const String ip = WiFi.softAPIP().toString();
  Serial.printf("  SSID: %s\n  IP:   %s\n", AP_SSID, ip.c_str());
  displayNetwork(ip);
}

// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.printf("\n=== GREENHOUSE CONTROLLER v%s ===\n\n", FIRMWARE_VERSION);

  // Relays to their safe state before anything else can run.
  controlBegin();

  esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);
  Serial.printf("Watchdog armed (%ds)\n", WATCHDOG_TIMEOUT_S);

  loadSettings();

  displayBegin();
  sensorsBegin();
  displaySplash(sensorData.numLeft, sensorData.numRight, sensorData.heaterDetected);

  startAccessPoint();
  webBegin();
  otaBegin(webServer());

  const unsigned long now = millis();
  lastHeaterRead = lastAirRead = lastDisplay = lastStatsLog = lastStatsSave = now;

  Serial.printf("\nAir band     : %.1fC - %.1fC\n", settings.tempMin, settings.tempMax);
  Serial.printf("Heater limits: %.1fC shed / %.1fC critical\n\n",
                settings.heaterMax, settings.heaterCritical);
  Serial.println(F("=== SYSTEM READY ===\n"));
}

// ----------------------------------------------------------------------------
void loop() {
  const unsigned long now = millis();

  webLoop();
  otaLoop();

  // Firmware is being written: the update path has already forced the outputs
  // safe, and touching the relays or the display now would fight it.
  if (otaInProgress()) {
    esp_task_wdt_reset();
    return;
  }

  // Heater zone: read fast, and act on the critical trips at the same rate.
  if (now - lastHeaterRead >= HEATER_TEMP_READ_INTERVAL) {
    lastHeaterRead = now;
    readHeaterTemperature();
    safetyTick(now);
  }

  // Air temperatures and the full control pass.
  if (now - lastAirRead >= AIR_TEMP_READ_INTERVAL) {
    lastAirRead = now;
    readAirTemperatures();
    controlSystem(now);
  }

  if (now - lastDisplay >= DISPLAY_UPDATE_INTERVAL) {
    lastDisplay = now;
    updateDisplay();
  }

  if (now - lastStatsLog >= STATS_LOG_INTERVAL) {
    lastStatsLog = now;
    logStatistics();
  }

  // Periodic persistence, so a power cut costs at most an hour of statistics.
  if (now - lastStatsSave >= STATS_SAVE_INTERVAL) {
    lastStatsSave = now;
    saveSettings();
  }

  // Debounced write after the operator stops pressing the +/- buttons.
  if (settingsChangedTime > 0 && (now - settingsChangedTime) >= EEPROM_SAVE_DELAY) {
    settingsChangedTime = 0;
    saveSettings();
  }

  esp_task_wdt_reset();
  delay(10);
}
