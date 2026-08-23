#include "config.h"
#include <Preferences.h>
#include "secrets.h"

// secrets.h is gitignored, so a fresh clone may not have every key yet. These
// fallbacks are the shipped defaults - deliberately obvious, deliberately
// meant to be changed from the WiFi page on first boot.
#ifndef AP_SSID
#define AP_SSID "Greenhouse"
#endif
#ifndef AP_PASSWORD
#define AP_PASSWORD "ChangeME"
#endif

const char* FIRMWARE_VERSION = "6.1.1";

Settings settings;
SystemStats stats;
unsigned long settingsChangedTime = 0;

static Preferences preferences;
static const char* NVS_NAMESPACE = "greenhouse";

void clampSetpoints() {
  settings.tempMin        = constrain(settings.tempMin,         5.0f, 20.0f);
  settings.tempMax        = constrain(settings.tempMax,        15.0f, 40.0f);
  settings.heaterMax      = constrain(settings.heaterMax,      25.0f, 45.0f);
  settings.heaterCritical = constrain(settings.heaterCritical, 40.0f, 60.0f);

  // Cooling must start well above where heating stops, or the two modes fight.
  settings.tempMin = min(settings.tempMin, settings.tempMax - MIN_SETPOINT_SPREAD);

  // The element limit must stay below the critical trip, with margin.
  settings.heaterMax = min(settings.heaterMax, settings.heaterCritical - 5.0f);
}

void loadSettings() {
  preferences.begin(NVS_NAMESPACE, false);

  settings.tempMin        = preferences.getFloat("temp_min", settings.tempMin);
  settings.tempMax        = preferences.getFloat("temp_max", settings.tempMax);
  settings.heaterMax      = preferences.getFloat("heater_max", settings.heaterMax);
  settings.heaterCritical = preferences.getFloat("heater_crit", settings.heaterCritical);
  settings.manualMode     = preferences.getBool("manual", false);

  stats.totalHeatingCycles   = preferences.getULong("heat_cycles", 0);
  stats.totalCoolingCycles   = preferences.getULong("cool_cycles", 0);
  stats.totalHeaterRuntime   = preferences.getULong("heat_runtime", 0);
  stats.safetyShutdownCount  = preferences.getULong("shutdowns", 0);
  stats.heaterSensorFailures = preferences.getULong("h_sens_fail", 0);
  stats.heaterCriticalEvents = preferences.getULong("h_critical", 0);
  stats.heaterSafetyEvents   = preferences.getULong("h_safety", 0);
  stats.airSensorFailures    = preferences.getULong("a_sens_fail", 0);
  stats.invalidReadingEvents = preferences.getULong("inv_read", 0);
  stats.minTempRecorded      = preferences.getFloat("t_min_rec", 999.0f);
  stats.maxTempRecorded      = preferences.getFloat("t_max_rec", -999.0f);

  preferences.end();

  // NVS contents are untrusted input: a stale pair from an older firmware must
  // not be able to put the controller into an illegal state.
  clampSetpoints();

  Serial.println(F("Settings loaded from NVS"));
  if (settings.manualMode) Serial.println(F("  MANUAL MODE restored - outputs start OFF"));
}

void saveSettings() {
  preferences.begin(NVS_NAMESPACE, false);

  preferences.putFloat("temp_min", settings.tempMin);
  preferences.putFloat("temp_max", settings.tempMax);
  preferences.putFloat("heater_max", settings.heaterMax);
  preferences.putFloat("heater_crit", settings.heaterCritical);
  preferences.putBool("manual", settings.manualMode);

  preferences.putULong("heat_cycles", stats.totalHeatingCycles);
  preferences.putULong("cool_cycles", stats.totalCoolingCycles);
  preferences.putULong("heat_runtime", stats.totalHeaterRuntime);
  preferences.putULong("shutdowns", stats.safetyShutdownCount);
  preferences.putULong("h_sens_fail", stats.heaterSensorFailures);
  preferences.putULong("h_critical", stats.heaterCriticalEvents);
  preferences.putULong("h_safety", stats.heaterSafetyEvents);
  preferences.putULong("a_sens_fail", stats.airSensorFailures);
  preferences.putULong("inv_read", stats.invalidReadingEvents);
  preferences.putFloat("t_min_rec", stats.minTempRecorded);
  preferences.putFloat("t_max_rec", stats.maxTempRecorded);

  preferences.end();
}

void resetStats() {
  stats = SystemStats();   // every counter back to its declared default
  saveSettings();
  Serial.println(F("Statistics reset to defaults"));
}

// ============================================================================
// NETWORK CONFIGURATION
// ============================================================================
static void copyField(char* dst, size_t cap, const char* src) {
  strlcpy(dst, src ? src : "", cap);
}

WifiConfig factoryWifiConfig() {
  WifiConfig cfg;
  cfg.staEnabled = false;
  copyField(cfg.apSsid, WIFI_SSID_LEN, AP_SSID);
  copyField(cfg.apPass, WIFI_PASS_LEN, AP_PASSWORD);
  cfg.staSsid[0] = 0;
  cfg.staPass[0] = 0;
  return cfg;
}

// A password the radio will actually accept: either open, or long enough for
// WPA2. Eight characters is the protocol's own floor, not an arbitrary one.
static bool passwordUsable(const char* pass) {
  const size_t len = strlen(pass);
  return len == 0 || (len >= 8 && len <= 63);
}

bool validateWifiConfig(WifiConfig& cfg) {
  const WifiConfig factory = factoryWifiConfig();
  bool ok = true;

  // The access point must always be reachable, so a bad AP field falls back to
  // the factory value rather than being rejected outright - there has to be a
  // way back in.
  const size_t apSsidLen = strlen(cfg.apSsid);
  if (apSsidLen == 0 || apSsidLen > 32) {
    copyField(cfg.apSsid, WIFI_SSID_LEN, factory.apSsid);
    ok = false;
  }
  if (!passwordUsable(cfg.apPass)) {
    copyField(cfg.apPass, WIFI_PASS_LEN, factory.apPass);
    ok = false;
  }

  // The station side has no such obligation: if it is unusable, just leave it
  // switched off. The board keeps hosting its own AP regardless.
  if (cfg.staEnabled) {
    const size_t staSsidLen = strlen(cfg.staSsid);
    if (staSsidLen == 0 || staSsidLen > 32 || !passwordUsable(cfg.staPass)) {
      cfg.staEnabled = false;
      ok = false;
    }
  }

  return ok;
}

void loadWifiConfig(WifiConfig& cfg) {
  const WifiConfig factory = factoryWifiConfig();
  preferences.begin(NVS_NAMESPACE, false);

  cfg.staEnabled = preferences.getBool("wifi_sta_en", factory.staEnabled);
  preferences.getString("ap_ssid",  cfg.apSsid,  WIFI_SSID_LEN);
  preferences.getString("ap_pass",  cfg.apPass,  WIFI_PASS_LEN);
  preferences.getString("sta_ssid", cfg.staSsid, WIFI_SSID_LEN);
  preferences.getString("sta_pass", cfg.staPass, WIFI_PASS_LEN);

  preferences.end();

  // Nothing stored yet - first boot, or NVS erased.
  if (cfg.apSsid[0] == 0) {
    copyField(cfg.apSsid, WIFI_SSID_LEN, factory.apSsid);
    copyField(cfg.apPass, WIFI_PASS_LEN, factory.apPass);
  }

  if (!validateWifiConfig(cfg)) {
    Serial.println(F("[WiFi] Stored configuration was invalid - fell back to factory"));
  }
}

void saveWifiConfig(const WifiConfig& cfg) {
  preferences.begin(NVS_NAMESPACE, false);

  preferences.putBool("wifi_sta_en", cfg.staEnabled);
  preferences.putString("ap_ssid",  cfg.apSsid);
  preferences.putString("ap_pass",  cfg.apPass);
  preferences.putString("sta_ssid", cfg.staSsid);
  preferences.putString("sta_pass", cfg.staPass);

  preferences.end();
  Serial.println(F("[WiFi] Configuration committed to NVS"));
}
