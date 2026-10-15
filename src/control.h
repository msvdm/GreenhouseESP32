#pragma once
#include <Arduino.h>

// ============================================================================
// RELAY CONTROL - the only code that writes the relay pins
// ============================================================================
// Heater: ON when avg <= heatOn, OFF when avg >= heatOn + hyst.
// Fans:   ON when avg >= coolOn, OFF when avg <= coolOn - hyst  (cooling),
//         and also whenever the heater is ON in "room" heating mode.
// No valid temperature: both automatic outputs OFF.
//
// Each relay can be overridden from the web UI (Auto / On / Off). Overrides
// are not saved - every boot starts in Auto with both relays off.

enum RelayMode : uint8_t { RELAY_AUTO = 0, RELAY_FORCE_ON, RELAY_FORCE_OFF };

struct ControlState {
  bool heater = false;
  bool fan = false;
  bool heatDemand = false;     // thermostat wants heat
  bool coolDemand = false;     // thermostat wants cooling
  RelayMode heaterMode = RELAY_AUTO;
  RelayMode fanMode = RELAY_AUTO;
  uint32_t heaterSec = 0;      // lifetime runtime totals
  uint32_t fanSec = 0;
};

extern ControlState ctrl;

void controlBegin();                       // relays off - call first in setup()
void controlUpdate(unsigned long now);     // re-evaluate both relays
void controlAllOff();                      // firmware update: drop both relays now
void controlSaveRuntime();

const char* relayModeName(RelayMode m);    // "auto" / "on" / "off"
bool relayModeParse(const String& s, RelayMode& out);

// Why the fan is running: "cool", "room", "manual" or "" when off.
const char* fanReason();
