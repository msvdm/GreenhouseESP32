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
//
// SCLK 18 and MOSI 23 are not named here because the Adafruit hardware-SPI
// constructor takes neither - it uses the VSPI defaults, which are those two
// pins. Note that GPIO 23 also drives the carrier board's status LED, so that
// LED flickers with SPI traffic and is useless as an indicator.
#define TFT_CS   21
#define TFT_RST  22
#define TFT_DC    2

// The carrier board's IO0 button, active-LOW. GPIO 0 is a strapping pin: held
// LOW through a reset it enters the serial bootloader, so this can only ever be
// a RUNTIME hold - by the time it is read here the bootloader has long since
// handed over and the pin is an ordinary input. Readings are unreliable while a
// USB-TTL module is attached, because its auto-reset circuit drives this pin.
#define BOOT_BUTTON_PIN 0
#define FACTORY_RESET_HOLD 5000UL          // 5 s hold, with a TFT countdown

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

// Plausibility band for an operator-entered simulated temperature. Nothing in
// a greenhouse legitimately reads outside this, and a typo that lands outside
// it should be refused rather than fed to the control logic.
#define SIM_TEMP_MIN -40.0f
#define SIM_TEMP_MAX 100.0f

// A simulated reading expires by itself. Manual mode is sticky because the
// operator can see the relays; a forgotten override is invisible and would go
// on feeding the controller invented weather indefinitely.
#define SIM_TIMEOUT 900000UL               // 15 min, then simulation clears

// Manual mode is a TEST mode: the zone protections above are deliberately not
// applied to it, and the critical trip is the only one left standing. That trip
// needs a readable heater-zone probe to fire, so heating with no probe at all is
// heating with nothing whatsoever limiting 2200 W. It is still allowed - it is
// the only way to exercise the relays on a bench with no sensors wired - but on
// a clock, and the clock resets the moment a real reading comes back.
#define MANUAL_BLIND_HEAT_LIMIT 300000UL   // 5 min unsupervised, then shed

// ============================================================================
// TIMING INTERVALS
// ============================================================================
#define AIR_TEMP_READ_INTERVAL 5000UL
#define HEATER_TEMP_READ_INTERVAL 1000UL   // Critical - check frequently
#define DISPLAY_UPDATE_INTERVAL 2000UL
#define STATS_LOG_INTERVAL 300000UL        // Log statistics every 5 min
#define STATS_SAVE_INTERVAL 3600000UL      // Persist statistics hourly
#define EEPROM_SAVE_DELAY 10000UL          // Debounce settings writes by 10 s

// How long a new WiFi configuration runs on trial before reverting itself.
// The same bargain a router offers: prove you can still reach the box, or it
// puts the working settings back without you.
#define WIFI_TRIAL_WINDOW 60000UL          // 60 s to confirm
#define WIFI_TRIAL_APPLY_DELAY 400UL       // Let the HTTP reply leave first

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

  // Operator intent, not machine state: "the controller should be under manual
  // control". Persisted like any other setting, so it survives a reboot. What
  // is deliberately NOT persisted is any output request - see control.h.
  bool manualMode = false;
};

extern Settings settings;

// ============================================================================
// NETWORK CONFIGURATION
// ============================================================================
// Editable from the web UI and stored in NVS. secrets.h supplies the FACTORY
// values only - once a configuration has been committed here, that is what the
// board comes up on.
#define WIFI_SSID_LEN 33   // 32 characters + NUL
#define WIFI_PASS_LEN 64   // 63 characters + NUL (WPA2 maximum)
#define WIFI_HOST_LEN 33   // mDNS label: 32 characters + NUL
#define WIFI_ADDR_LEN 16   // "255.255.255.255" + NUL

struct WifiConfig {
  bool staEnabled = false;              // Join an existing network as well
  char apSsid[WIFI_SSID_LEN]  = { 0 };  // The board's own access point
  char apPass[WIFI_PASS_LEN]  = { 0 };
  char staSsid[WIFI_SSID_LEN] = { 0 };  // The network to join, when enabled
  char staPass[WIFI_PASS_LEN] = { 0 };

  // How the board is addressed. The AP always has a numeric address - mDNS is
  // an ADDITIONAL name, never a replacement - so both are stored rather than
  // one field that changes meaning. Flipping the switch on the settings page
  // therefore cannot lose the other value, and if .local fails to resolve on
  // some client the numeric address is still exactly where it was.
  char apIp[WIFI_ADDR_LEN]    = { 0 };  // AP address, e.g. "192.168.4.1"
  bool mdnsEnabled = false;             // Announce <hostname>.local
  char hostname[WIFI_HOST_LEN] = { 0 }; // "greenhouse" -> greenhouse.local
};

// The compiled-in factory configuration, from secrets.h.
WifiConfig factoryWifiConfig();

void loadWifiConfig(WifiConfig& cfg);
void saveWifiConfig(const WifiConfig& cfg);

// The direct analogue of clampSetpoints(), and it exists for the same reason:
// NVS and HTTP form fields are both untrusted input. An SSID must be 1-32
// characters and a password must be either empty (open network) or 8-63, which
// is what the radio itself will accept. Anything else falls back to the
// factory value rather than being silently truncated into something that
// almost works. Returns false if it had to change anything.
bool validateWifiConfig(WifiConfig& cfg);

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
