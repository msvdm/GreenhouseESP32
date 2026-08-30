#include "button.h"
#include "auth.h"
#include "config.h"
#include "control.h"
#include "display.h"
#include "net.h"
#include "ota.h"

// A press has to survive this long before the countdown is drawn. Without it a
// single noisy sample repaints the whole screen for one frame, which reads as a
// glitching display rather than as a button being ignored.
#define HOLD_DEBOUNCE 400UL

static bool holding = false;
static bool fired = false;              // reset done; waiting for release
static unsigned long holdStart = 0;
static int lastShown = -1;

void buttonBegin() {
  // The carrier board pulls IO0 up for the bootloader; the internal pull-up
  // makes the reading well-defined regardless.
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
}

static void doFactoryReset(unsigned long now) {
  Serial.println(F("\n[BUTTON] Factory reset - defaults restored"));

  // Setpoints first, then back to automatic through the proper path so the
  // manual output requests are cleared by the code that owns them.
  settings = Settings();
  clampSetpoints();
  setManualMode(false, now);
  saveSettings();

  authFactoryReset();

  // Last: this restarts the access point and drops every client, including
  // whoever might be watching the page.
  netFactoryResetNow();

  // Read back from the live configuration, which netFactoryResetNow() has just
  // set to the factory values - so the screen reports what the board actually
  // came up on rather than what it was asked to.
  displayResetDone(netLiveConfig().apSsid, authDefaultPassword());
}

void buttonLoop(unsigned long now) {
  // A firmware write has already forced the outputs safe and released the
  // watchdog. Resetting the network out from under an upload in progress would
  // drop the connection carrying it.
  if (otaInProgress()) {
    holding = false;
    fired = false;
    return;
  }

  const bool down = (digitalRead(BOOT_BUTTON_PIN) == LOW);

  if (!down) {
    // Repaint only if a countdown was actually drawn. A reset that fired leaves
    // its own confirmation on screen, and that should stay put.
    if (holding && !fired && lastShown >= 0) displayInvalidate();
    holding = false;
    fired = false;
    lastShown = -1;
    return;
  }

  if (!holding) {
    holding = true;
    fired = false;
    holdStart = now;
    lastShown = -1;
    return;
  }

  if (fired) return;                       // done - wait for the release

  // Elapsed difference, never (now - WINDOW): the latter underflows for the
  // first five seconds after boot and again at the millis() wrap.
  const unsigned long held = now - holdStart;

  if (held >= FACTORY_RESET_HOLD) {
    fired = true;
    doFactoryReset(now);
    return;
  }

  if (held < HOLD_DEBOUNCE) return;

  const int secondsLeft = (int)((FACTORY_RESET_HOLD - held) / 1000) + 1;
  if (secondsLeft != lastShown) {
    lastShown = secondsLeft;
    displayHoldReset(secondsLeft);
  }
}
