#pragma once

// ============================================================================
// ONBOARD STATUS LED (GPIO 23)
// ============================================================================
//   short blink every 2 s  all good (and proves loop() is running)
//   slow blink, 1 Hz       home WiFi configured but not connected
//   fast blink, 4 Hz       no valid temperature - automatic relays are off
//   solid on               firmware update in progress
// The most important condition wins.

void ledBegin();
void ledUpdate(unsigned long now);
