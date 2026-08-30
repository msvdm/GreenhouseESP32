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

// 192.168.4.1 is the ESP32 SoftAP default. Keeping it as the factory value
// means a reset always lands somewhere documented on the board and in every
// tutorial the operator is likely to be holding.
#ifndef AP_ADDRESS
#define AP_ADDRESS "192.168.4.1"
#endif
#ifndef MDNS_HOSTNAME
#define MDNS_HOSTNAME "greenhouse"
#endif

const char* FIRMWARE_VERSION = "7.0.1";

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
  copyField(cfg.apIp, WIFI_ADDR_LEN, AP_ADDRESS);
  copyField(cfg.hostname, WIFI_HOST_LEN, MDNS_HOSTNAME);
  cfg.mdnsEnabled = true;
  return cfg;
}

// A dotted quad the SoftAP can actually be given: four octets, and a last one
// that is neither the network nor the broadcast address. Rejecting .0 and .255
// matters because softAPConfig() accepts them and then hands out a DHCP range
// no client can use - the board simply stops being reachable, with no error.
static bool addressUsable(const char* addr) {
  int a, b, c, d;
  char trailing;
  if (sscanf(addr, "%d.%d.%d.%d%c", &a, &b, &c, &d, &trailing) != 4) return false;
  if (a < 1 || a > 254) return false;
  if (b < 0 || b > 255) return false;
  if (c < 0 || c > 255) return false;
  if (d < 1 || d > 254) return false;
  return true;
}

// A single DNS label, which is all an mDNS hostname is: letters, digits and
// interior hyphens. Lowercased in place because mDNS is case-insensitive but
// clients are not always consistent about it, and a name that resolves from one
// phone and not another is worse than one that never worked.
static bool hostnameUsable(char* host) {
  const size_t len = strlen(host);
  if (len == 0 || len > 32) return false;
  if (host[0] == '-' || host[len - 1] == '-') return false;
  for (size_t i = 0; i < len; i++) {
    char ch = host[i];
    if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
    if (!ok) return false;
    host[i] = ch;
  }
  return true;
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

  // The address is an AP field and gets the AP's treatment: fall back rather
  // than reject, because there has to be a way back in.
  if (!addressUsable(cfg.apIp)) {
    copyField(cfg.apIp, WIFI_ADDR_LEN, factory.apIp);
    ok = false;
  }

  // A bad hostname only costs the .local name, never reachability, so this one
  // could have been switched off instead. It falls back for consistency: every
  // other field here ends up valid rather than absent.
  if (!hostnameUsable(cfg.hostname)) {
    copyField(cfg.hostname, WIFI_HOST_LEN, factory.hostname);
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

  cfg.staEnabled   = preferences.getBool("wifi_sta_en", factory.staEnabled);
  cfg.mdnsEnabled  = preferences.getBool("mdns_en", factory.mdnsEnabled);
  preferences.getString("ap_ssid",  cfg.apSsid,  WIFI_SSID_LEN);
  preferences.getString("ap_pass",  cfg.apPass,  WIFI_PASS_LEN);
  preferences.getString("sta_ssid", cfg.staSsid, WIFI_SSID_LEN);
  preferences.getString("sta_pass", cfg.staPass, WIFI_PASS_LEN);
  preferences.getString("ap_ip",    cfg.apIp,    WIFI_ADDR_LEN);
  preferences.getString("host",     cfg.hostname, WIFI_HOST_LEN);

  preferences.end();

  // Nothing stored yet - first boot, or NVS erased.
  if (cfg.apSsid[0] == 0) {
    copyField(cfg.apSsid, WIFI_SSID_LEN, factory.apSsid);
    copyField(cfg.apPass, WIFI_PASS_LEN, factory.apPass);
  }

  // Upgrading from a firmware that had no address or hostname: these keys are
  // simply absent, which is not corruption. Seed them from the factory values
  // rather than letting validateWifiConfig() report a fault that isn't one.
  if (cfg.apIp[0] == 0)     copyField(cfg.apIp, WIFI_ADDR_LEN, factory.apIp);
  if (cfg.hostname[0] == 0) copyField(cfg.hostname, WIFI_HOST_LEN, factory.hostname);

  if (!validateWifiConfig(cfg)) {
    Serial.println(F("[WiFi] Stored configuration was invalid - fell back to factory"));
  }
}

void saveWifiConfig(const WifiConfig& cfg) {
  preferences.begin(NVS_NAMESPACE, false);

  preferences.putBool("wifi_sta_en", cfg.staEnabled);
  preferences.putBool("mdns_en", cfg.mdnsEnabled);
  preferences.putString("ap_ssid",  cfg.apSsid);
  preferences.putString("ap_pass",  cfg.apPass);
  preferences.putString("sta_ssid", cfg.staSsid);
  preferences.putString("sta_pass", cfg.staPass);
  preferences.putString("ap_ip",    cfg.apIp);
  preferences.putString("host",     cfg.hostname);

  preferences.end();
  Serial.println(F("[WiFi] Configuration committed to NVS"));
}
