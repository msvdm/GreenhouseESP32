#include "control.h"
#include "config.h"
#include "sensors.h"
#include "settings.h"

ControlState ctrl;

static unsigned long lastTick = 0;
static uint32_t heaterMs = 0;   // sub-second remainders for the runtime totals
static uint32_t fanMs = 0;

static void writeRelays() {
  digitalWrite(PIN_RELAY_HEATER, ctrl.heater ? HIGH : LOW);
  digitalWrite(PIN_RELAY_FAN, ctrl.fan ? HIGH : LOW);
}

void controlBegin() {
  digitalWrite(PIN_RELAY_HEATER, LOW);
  digitalWrite(PIN_RELAY_FAN, LOW);
  pinMode(PIN_RELAY_HEATER, OUTPUT);
  pinMode(PIN_RELAY_FAN, OUTPUT);
  runtimeLoad(ctrl.heaterSec, ctrl.fanSec);
  lastTick = millis();
}

static void accumulate(unsigned long now) {
  const uint32_t dt = now - lastTick;
  lastTick = now;
  if (ctrl.heater) { heaterMs += dt; ctrl.heaterSec += heaterMs / 1000; heaterMs %= 1000; }
  if (ctrl.fan)    { fanMs += dt;    ctrl.fanSec    += fanMs / 1000;    fanMs %= 1000; }
}

void controlUpdate(unsigned long now) {
  accumulate(now);

  // Thermostat with hysteresis. Inside the band the previous demand holds.
  const float t = sensorsAverage();
  if (isnan(t)) {
    ctrl.heatDemand = false;
    ctrl.coolDemand = false;
  } else {
    if (t <= settings.heatOn)                    ctrl.heatDemand = true;
    else if (t >= settings.heatOn + settings.hyst) ctrl.heatDemand = false;

    if (t >= settings.coolOn)                    ctrl.coolDemand = true;
    else if (t <= settings.coolOn - settings.hyst) ctrl.coolDemand = false;
  }

  bool heater = ctrl.heatDemand;
  if (ctrl.heaterMode == RELAY_FORCE_ON)  heater = true;
  if (ctrl.heaterMode == RELAY_FORCE_OFF) heater = false;

  // Fan follows the ACTUAL heater state in room mode, so a manual heater-on
  // also gets convection.
  bool fan = ctrl.coolDemand || (settings.roomHeating && heater);
  if (ctrl.fanMode == RELAY_FORCE_ON)  fan = true;
  if (ctrl.fanMode == RELAY_FORCE_OFF) fan = false;

  if (heater != ctrl.heater || fan != ctrl.fan) {
    Serial.printf("[control] avg %.2f C -> heater %s, fan %s\n", t,
                  heater ? "ON" : "off", fan ? "ON" : "off");
  }
  ctrl.heater = heater;
  ctrl.fan = fan;
  writeRelays();
}

void controlAllOff() {
  accumulate(millis());
  ctrl.heater = false;
  ctrl.fan = false;
  writeRelays();
}

void controlSaveRuntime() { runtimeSave(ctrl.heaterSec, ctrl.fanSec); }

const char* relayModeName(RelayMode m) {
  switch (m) {
    case RELAY_FORCE_ON:  return "on";
    case RELAY_FORCE_OFF: return "off";
    default:              return "auto";
  }
}

bool relayModeParse(const String& s, RelayMode& out) {
  if (s == "auto") { out = RELAY_AUTO; return true; }
  if (s == "on")   { out = RELAY_FORCE_ON; return true; }
  if (s == "off")  { out = RELAY_FORCE_OFF; return true; }
  return false;
}

const char* fanReason() {
  if (!ctrl.fan) return "";
  if (ctrl.fanMode == RELAY_FORCE_ON) return "manual";
  if (ctrl.coolDemand) return "cool";
  return "room";
}
