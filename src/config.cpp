#include "config.h"
#include <Preferences.h>

const char* FIRMWARE_VERSION = "6.0.0";

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
}

void saveSettings() {
  preferences.begin(NVS_NAMESPACE, false);

  preferences.putFloat("temp_min", settings.tempMin);
  preferences.putFloat("temp_max", settings.tempMax);
  preferences.putFloat("heater_max", settings.heaterMax);
  preferences.putFloat("heater_crit", settings.heaterCritical);

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
