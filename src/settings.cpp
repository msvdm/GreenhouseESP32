#include "settings.h"
#include <Preferences.h>
#include "secrets.h"

#ifndef AP_SSID
#define AP_SSID "Green"
#endif
#ifndef AP_PASSWORD
#define AP_PASSWORD "ChangeME"
#endif

Settings settings;

static Preferences prefs;
static const char* NS = "greenhouse";

const char* settingsValidate(float heatOn, float coolOn, float hyst) {
  if (isnan(heatOn) || heatOn < 0.0f || heatOn > 35.0f) return "Heating setpoint must be 0-35 C";
  if (isnan(coolOn) || coolOn < 5.0f || coolOn > 50.0f) return "Fan setpoint must be 5-50 C";
  if (isnan(hyst) || hyst < 0.2f || hyst > 5.0f)       return "Hysteresis must be 0.2-5 C";
  // The heater's OFF point must stay below the fans' OFF point, otherwise
  // heating would switch the fans on for cooling and the two would fight.
  if (heatOn + hyst >= coolOn - hyst) return "Fan setpoint must be above heating setpoint + 2x hysteresis";
  return nullptr;
}

void settingsLoad() {
  prefs.begin(NS, true);
  // First boot after 7.x: start from the old min/max band.
  const float oldMin = prefs.getFloat("temp_min", 10.0f);
  const float oldMax = prefs.getFloat("temp_max", 25.0f);
  Settings s;
  s.heatOn      = prefs.getFloat("heat_on", oldMin);
  s.coolOn      = prefs.getFloat("cool_on", oldMax);
  s.hyst        = prefs.getFloat("hyst", 0.5f);
  s.roomHeating = prefs.getBool("room_heat", false);
  prefs.end();

  if (settingsValidate(s.heatOn, s.coolOn, s.hyst) == nullptr) {
    settings = s;
  } else {
    settings = Settings();   // stored values unusable - defaults
    settings.roomHeating = s.roomHeating;
  }
}

void settingsSave() {
  prefs.begin(NS, false);
  prefs.putFloat("heat_on", settings.heatOn);
  prefs.putFloat("cool_on", settings.coolOn);
  prefs.putFloat("hyst", settings.hyst);
  prefs.putBool("room_heat", settings.roomHeating);
  prefs.end();
}

// ----------------------------------------------------------------------------
void wifiFactoryAp(WifiSettings& w) {
  strlcpy(w.apSsid, AP_SSID, sizeof(w.apSsid));
  strlcpy(w.apPass, AP_PASSWORD, sizeof(w.apPass));
}

void wifiLoad(WifiSettings& w) {
  prefs.begin(NS, true);
  w.staEnabled = prefs.getBool("wifi_sta_en", false);
  prefs.getString("sta_ssid", w.staSsid, sizeof(w.staSsid));
  prefs.getString("sta_pass", w.staPass, sizeof(w.staPass));
  prefs.getString("ap_ssid", w.apSsid, sizeof(w.apSsid));
  prefs.getString("ap_pass", w.apPass, sizeof(w.apPass));
  prefs.getString("host", w.hostname, sizeof(w.hostname));
  prefs.end();

  const size_t apPassLen = strlen(w.apPass);
  if (w.apSsid[0] == 0 || (apPassLen > 0 && apPassLen < 8)) wifiFactoryAp(w);
  if (w.hostname[0] == 0) strlcpy(w.hostname, "greenhouse", sizeof(w.hostname));
  if (w.staSsid[0] == 0) w.staEnabled = false;
}

void wifiSave(const WifiSettings& w) {
  prefs.begin(NS, false);
  prefs.putBool("wifi_sta_en", w.staEnabled);
  prefs.putString("sta_ssid", w.staSsid);
  prefs.putString("sta_pass", w.staPass);
  prefs.putString("ap_ssid", w.apSsid);
  prefs.putString("ap_pass", w.apPass);
  prefs.putString("host", w.hostname);
  prefs.end();
}

// ----------------------------------------------------------------------------
void runtimeLoad(uint32_t& heaterSec, uint32_t& fanSec) {
  prefs.begin(NS, true);
  heaterSec = prefs.getULong("rt_heat_s", 0);
  fanSec    = prefs.getULong("rt_fan_s", 0);
  prefs.end();
}

void runtimeSave(uint32_t heaterSec, uint32_t fanSec) {
  prefs.begin(NS, false);
  prefs.putULong("rt_heat_s", heaterSec);
  prefs.putULong("rt_fan_s", fanSec);
  prefs.end();
}
