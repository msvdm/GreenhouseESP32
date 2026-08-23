#pragma once
#include <Arduino.h>

// ============================================================================
// GREENHOUSE CONTROLLER - BOARD CONFIGURATION AND PERSISTENT STATE
// ============================================================================
// Safety-critical control of a 2200W heating element via contactor.
// Target board: ESP32-WROOM-32E on a 2-channel relay carrier (10A / 250VAC).
// ============================================================================

extern const char* FIRMWARE_VERSION;

// ============================================================================
// PIN DEFINITIONS
// ============================================================================
// The two onboard relays are fixed by the carrier board at GPIO 16/17, and
// both relay inputs are active-HIGH: driving the pin HIGH energises the coil.
#define FAN_RELAY_PIN     16    // Relay CH1 - circulation fan
#define HEATER_RELAY_PIN  17    // Relay CH2 - heater contactor (2200W element)

#define RELAY_ON  HIGH
#define RELAY_OFF LOW

// OneWire buses (DS18B20 - each bus needs its own 4k7 pull-up to 3V3).
#define LEFT_SENSORS_BUS   4
#define RIGHT_SENSORS_BUS 15
#define HEATER_SENSOR_BUS  5

// TFT display. CS and RST sit on 21/22 because GPIO 5 and 17 are now the
// heater sensor bus and the heater relay respectively.
#define TFT_CS   21
#define TFT_RST  22
#define TFT_DC    2
#define TFT_SCLK 18
#define TFT_MOSI 23

#define MAX_SENSORS_PER_SIDE 3

// ============================================================================
// CONTROL BAND
// ============================================================================
#define HYSTERESIS 0.5f              // Temperature deadband
#define MODE_CHANGE_HYSTERESIS 1.0f  // Extra margin to stop mode oscillation
#define HEATING_TARGET_OFFSET 2.0f   // Overshoot past tempMin to avoid short cycling
#define HEATER_ARM_MARGIN 2.0f       // Heater zone must be this far below its limit to arm
#define MIN_SETPOINT_SPREAD 5.0f     // Required gap between tempMin and tempMax

// ============================================================================
// SAFETY TIMERS - PREVENT RUNAWAY CONDITIONS
// ============================================================================
#define MAX_HEATER_RUNTIME 1800000UL       // 30 min max continuous heating
#define MIN_HEATER_OFF_TIME 300000UL       // 5 min minimum between heating cycles
#define FAN_STARTUP_DELAY 5000UL           // 5 s for fan to reach full airflow
#define FAN_COOLDOWN_TIME 60000UL          // 60 s fan purge after heater off
#define MAX_HEATING_CYCLES_PER_HOUR 6      // Prevent excessive cycling
#define SENSOR_FAILURE_GRACE_PERIOD 30000UL  // 30 s grace before shutdown
#define CYCLE_WINDOW 3600000UL             // Sliding window for the cycle limit

// ============================================================================
// TIMING INTERVALS
// ============================================================================
#define AIR_TEMP_READ_INTERVAL 5000UL
#define HEATER_TEMP_READ_INTERVAL 1000UL   // Critical - check frequently
#define DISPLAY_UPDATE_INTERVAL 2000UL
#define STATS_LOG_INTERVAL 300000UL        // Log statistics every 5 min
#define STATS_SAVE_INTERVAL 3600000UL      // Persist statistics hourly
#define EEPROM_SAVE_DELAY 10000UL          // Debounce settings writes by 10 s

#define WATCHDOG_TIMEOUT_S 20

// ============================================================================
// OPERATOR-ADJUSTABLE SETPOINTS
// ============================================================================
// Lowercase members on a struct, deliberately: these are runtime state loaded
// from NVS and edited from the web UI, not compile-time constants. The former
// SCREAMING_CASE globals read as constants and were anything but.
struct Settings {
  float tempMin        = 10.0f;   // Start heating below this
  float tempMax        = 25.0f;   // Start cooling above this
  float heaterMax      = 35.0f;   // Heater zone limit - sheds the element
  float heaterCritical = 50.0f;   // Fan's rated ambient - sheds everything
};

extern Settings settings;

// ============================================================================
// STATISTICS
// ============================================================================
struct SystemStats {
  unsigned long totalHeatingCycles = 0;
  unsigned long totalCoolingCycles = 0;
  unsigned long totalHeaterRuntime = 0;
  float minTempRecorded = 999.0f;
  float maxTempRecorded = -999.0f;
  unsigned long safetyShutdownCount = 0;
  unsigned long heaterSensorFailures = 0;
  unsigned long heaterCriticalEvents = 0;
  unsigned long heaterSafetyEvents = 0;
  unsigned long airSensorFailures = 0;
  unsigned long invalidReadingEvents = 0;
};

extern SystemStats stats;

// Set by the web UI on edit; main loop persists once the value stops moving.
extern unsigned long settingsChangedTime;

void loadSettings();
void saveSettings();
void resetStats();

// Enforce every setpoint relationship the individual ranges cannot express.
// Applied after each edit AND after loadSettings(), so stale or partially
// written NVS contents can never produce a contradictory configuration.
void clampSetpoints();
