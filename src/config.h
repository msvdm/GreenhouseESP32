#pragma once
#include <Arduino.h>

// ============================================================================
// GREENHOUSE CONTROLLER - board pins and fixed constants
// ============================================================================
// Board: ESP32-WROOM-32E 2-channel relay module ("ESP32 Relay x2").
// Relays are active-HIGH. The board has no USB port - updates go over WiFi.

#define FW_VERSION "8.1.0"

// Onboard relays, fixed by the board.
#define PIN_RELAY_FAN     16   // Relay 1 - ceiling fans
#define PIN_RELAY_HEATER  17   // Relay 2 - heater contactor coil (radiant heaters)

// DS18B20 sensor lines, each with its own 4k7 pull-up to 3V3. Any number of
// probes per line; they are discovered automatically.
#define PIN_LINE_1  4
#define PIN_LINE_2  15
#define MAX_PROBES_PER_LINE 8

// ST7735 160x128 on hardware SPI (VSPI) with remapped pins, so that GPIO 23
// (the VSPI default MOSI) is free for the onboard LED. None of these are
// strapping pins. GPIO 19 stays unused: SPI.begin() claims it as MISO input.
#define PIN_TFT_SCLK 18
#define PIN_TFT_MOSI 26
#define PIN_TFT_CS   25
#define PIN_TFT_DC   27
#define PIN_TFT_RST  33

// Onboard LED (active-HIGH), used as a status light - see led.h.
#define PIN_LED 23

// IO0 button. Held for 5 s while running: web password and access point go
// back to factory values. It never touches a relay.
#define PIN_BUTTON          0
#define FACTORY_RESET_HOLD  5000UL

// Timing
#define SENSOR_INTERVAL     2000UL     // read + control pass
#define SENSOR_RESCAN       30000UL    // re-enumerate probes (hot-plug)
#define SENSOR_STALE        30000UL    // a line with no valid probe for this long is "lost"
#define DISPLAY_INTERVAL    1000UL
#define RUNTIME_SAVE        1800000UL  // persist relay runtime counters every 30 min
#define WATCHDOG_TIMEOUT_S  30         // reboot (relays off) if loop() hangs
