// ============================================================================
// GREENHOUSE CONTROLLER
// ============================================================================
// Two DS18B20 lines -> average temperature -> heater contactor + ceiling fans.
//
//   config.h    pins and timing
//   settings.*  NVS-backed setpoints, WiFi and runtime totals
//   sensors.*   probe discovery and averaging
//   control.*   thermostat + manual override - the only relay writer
//   display.*   ST7735 status screen
//   led.*       onboard status LED
//   net.*       WiFi (AP + home network), mDNS, espota
//   auth.*      web password and sessions
//   web.*       pages, JSON API, browser firmware upload
// ============================================================================

#include <Arduino.h>
#include <esp_task_wdt.h>
#include "auth.h"
#include "config.h"
#include "control.h"
#include "display.h"
#include "led.h"
#include "net.h"
#include "sensors.h"
#include "settings.h"
#include "web.h"

static unsigned long lastSensor = 0;
static unsigned long lastDisplay = 0;
static unsigned long lastRuntimeSave = 0;

// IO0 held for FACTORY_RESET_HOLD: web password + access point to factory.
// Returns true while the button is held (the screen shows the countdown).
static bool buttonLoop(unsigned long now) {
  static unsigned long pressedAt = 0;
  static int shown = -1;

  if (digitalRead(PIN_BUTTON) == HIGH) {
    if (pressedAt) displayUpdate();   // released early - back to normal screen
    pressedAt = 0;
    shown = -1;
    return false;
  }
  if (!pressedAt) pressedAt = now ? now : 1;

  const unsigned long held = now - pressedAt;
  if (held < FACTORY_RESET_HOLD) {
    const int left = (FACTORY_RESET_HOLD - held + 999) / 1000;
    if (left != shown) {
      shown = left;
      char buf[24];
      snprintf(buf, sizeof(buf), "reset in %d s", left);
      displayMessage("HOLD IO0", buf, "release to cancel", 0xFFE0);
    }
    return true;
  }

  authFactoryReset();
  WifiSettings w = netConfig();
  wifiFactoryAp(w);
  netSave(w);
  Serial.println(F("[reset] web password and access point back to factory"));
  displayMessage("RESET DONE", "password + AP restored", "rebooting...", 0x07E0);
  controlAllOff();
  controlSaveRuntime();
  delay(2000);
  ESP.restart();
  return true;
}

void setup() {
  controlBegin();   // relays off before anything else
  Serial.begin(115200);
  Serial.printf("\n=== Greenhouse controller v%s ===\n", FW_VERSION);

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  ledBegin();
  settingsLoad();
  displayBegin();
  sensorsBegin();
  netBegin();
  webBegin();

  // If loop() ever hangs, reboot - controlBegin() then drops both relays.
  esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);

  lastSensor = lastDisplay = lastRuntimeSave = millis();
  Serial.printf("Heating <= %.1f C, fans >= %.1f C, hysteresis %.1f C, %s heating\n",
                settings.heatOn, settings.coolOn, settings.hyst,
                settings.roomHeating ? "room" : "zone");
}

void loop() {
  const unsigned long now = millis();
  esp_task_wdt_reset();

  netLoop();
  webLoop();
  ledUpdate(now);
  if (otaActive()) return;   // relays are off and the screen shows progress

  if (now - lastSensor >= SENSOR_INTERVAL) {
    lastSensor = now;
    sensorsUpdate(now);
    controlUpdate(now);
  }

  const bool holding = buttonLoop(now);
  if (!holding && now - lastDisplay >= DISPLAY_INTERVAL) {
    lastDisplay = now;
    displayUpdate();
  }

  if (now - lastRuntimeSave >= RUNTIME_SAVE) {
    lastRuntimeSave = now;
    controlSaveRuntime();
  }

  delay(5);
}
