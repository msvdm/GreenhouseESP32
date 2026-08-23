#pragma once
#include <WebServer.h>

// ============================================================================
// OVER-THE-AIR FIRMWARE UPDATE
// ============================================================================
// Two independent routes onto the device, because the target board has no USB:
//
//   1. ArduinoOTA  - `pio run -t upload --upload-port <ip>` from a workstation.
//   2. POST /update - browser file upload, needs no toolchain at all.
//
// Both share the same safety preamble: the heater is shed and the fans are
// left running before a single byte is written, and the task watchdog is
// released because an update blocks the control loop for far longer than its
// timeout. If an update fails, the ESP32 bootloader keeps running the existing
// image from the other OTA slot - a corrupt upload cannot brick the board.
void otaBegin(WebServer& server);

// Must be called from loop() to service ArduinoOTA.
void otaLoop();

// True while an update is in progress; the main loop suspends normal work.
bool otaInProgress();
