#pragma once
#include <Arduino.h>

// ============================================================================
// PERSISTENT SETTINGS (NVS namespace "greenhouse")
// ============================================================================
// The WiFi and web-password keys are the same ones firmware 7.x used, so an OTA
// update from 7.x keeps the board on its network and keeps its password.

struct Settings {
  float heatOn = 10.0f;      // heater ON at or below this
  float coolOn = 25.0f;      // fans ON at or above this
  float hyst   = 0.5f;       // heater OFF at heatOn+hyst, fans OFF at coolOn-hyst
  bool  roomHeating = false; // false: zone (heater only), true: room (heater + fans)
};

struct WifiSettings {
  bool staEnabled = false;
  char staSsid[33] = "";
  char staPass[64] = "";
  char apSsid[33]  = "";
  char apPass[64]  = "";
  char hostname[33] = "";
};

extern Settings settings;

void settingsLoad();
void settingsSave();

// Returns nullptr if the values are acceptable, otherwise a reason.
const char* settingsValidate(float heatOn, float coolOn, float hyst);

void wifiLoad(WifiSettings& w);
void wifiSave(const WifiSettings& w);
void wifiFactoryAp(WifiSettings& w);   // reset AP name/password to factory values

// Relay runtime totals in seconds.
void runtimeLoad(uint32_t& heaterSec, uint32_t& fanSec);
void runtimeSave(uint32_t heaterSec, uint32_t fanSec);
