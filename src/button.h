#pragma once
#include <Arduino.h>

// ============================================================================
// IO0 FACTORY RESET BUTTON
// ============================================================================
// This board has no USB port, so every credential it holds has to be
// recoverable without one. Holding the carrier board's IO0 button for
// FACTORY_RESET_HOLD puts the web password, the access point and the setpoints
// back to their defaults. Statistics are deliberately left alone - they are a
// maintenance record, nothing about them can lock anybody out, and /stats has
// its own reset button.
//
// WHY A RUNTIME HOLD RATHER THAN HOLD-AT-BOOT. GPIO 0 is a strapping pin:
// holding it LOW through a reset selects the serial bootloader, so a
// hold-at-boot reset would never reach this firmware at all. By the time
// buttonLoop() runs, the bootloader has handed over and the pin is an ordinary
// input.
//
// This module never touches a relay. A factory reset is a credential
// operation, not an emergency stop, and setHeater()/setFans() in control.cpp
// remain the only functions in the firmware that write a relay pin.
void buttonBegin();

// Non-blocking, called from loop() on every pass. Any HIGH reading abandons the
// hold, which makes a noise glitch fail toward NOT resetting.
void buttonLoop(unsigned long now);
