#include "net.h"
#include <WiFi.h>
#include "display.h"

// committed - what is in NVS, and what the board will boot on.
// live      - what the radio is running right now.
// During a trial the two differ, and only netConfirm() closes the gap.
static WifiConfig committed;
static WifiConfig live;

static bool trialRunning = false;
static unsigned long trialStart = 0;

static bool applyPending = false;
static unsigned long applyRequestedAt = 0;
static WifiConfig candidate;

// ----------------------------------------------------------------------------
static void applyRadio(const WifiConfig& cfg) {
  WiFi.mode(cfg.staEnabled ? WIFI_AP_STA : WIFI_AP);

  // Tear the AP down and bring it back so clients are forced to reassociate
  // with the new credentials. That is not a side effect, it IS the test: if
  // the operator cannot get back on, the trial should fail.
  WiFi.softAPdisconnect(false);
  delay(100);
  WiFi.softAP(cfg.apSsid, cfg.apPass[0] ? cfg.apPass : nullptr);

  if (cfg.staEnabled) {
    WiFi.begin(cfg.staSsid, cfg.staPass);
  } else {
    WiFi.disconnect(false, true);
  }

  live = cfg;

  Serial.printf("[WiFi] AP  \"%s\" at %s\n", live.apSsid, WiFi.softAPIP().toString().c_str());
  if (live.staEnabled) Serial.printf("[WiFi] STA \"%s\" connecting\n", live.staSsid);

  displayNetwork(WiFi.softAPIP().toString());
}

// ----------------------------------------------------------------------------
void netBegin() {
  Serial.println(F("\n[WiFi] Starting network"));
  loadWifiConfig(committed);
  applyRadio(committed);
}

void netLoop(unsigned long now) {
  // A queued change waits briefly so the browser gets its acknowledgement
  // before the access point drops it.
  if (applyPending && (now - applyRequestedAt) >= WIFI_TRIAL_APPLY_DELAY) {
    applyPending = false;
    trialRunning = true;
    trialStart = now;
    Serial.printf("[WiFi] Trial started - %lus to confirm\n", WIFI_TRIAL_WINDOW / 1000);
    applyRadio(candidate);
    return;
  }

  if (!trialRunning) return;

  // Elapsed difference, never (now - WINDOW): the latter underflows for the
  // first minute after boot and at the millis() wrap.
  if ((now - trialStart) < WIFI_TRIAL_WINDOW) return;

  trialRunning = false;
  Serial.println(F("[WiFi] Not confirmed in time - reverting to previous settings"));
  applyRadio(committed);
}

// ----------------------------------------------------------------------------
bool netRequestTrial(const WifiConfig& c) {
  candidate = c;

  // Untrusted input from a form field. A configuration that cannot be made
  // valid is refused rather than quietly corrected into something else.
  if (!validateWifiConfig(candidate)) return false;

  applyRequestedAt = millis();
  applyPending = true;
  return true;
}

bool netRequestFactoryReset() {
  // Deliberately runs as a trial like any other change: a misfired reset is
  // then exactly as recoverable as a mistyped password.
  return netRequestTrial(factoryWifiConfig());
}

bool netConfirm() {
  if (!trialRunning) return false;
  trialRunning = false;
  committed = live;
  saveWifiConfig(committed);
  return true;
}

bool netTrialActive() { return trialRunning || applyPending; }

unsigned long netTrialSecondsLeft() {
  if (applyPending)  return WIFI_TRIAL_WINDOW / 1000;
  if (!trialRunning) return 0;
  const unsigned long elapsed = millis() - trialStart;
  if (elapsed >= WIFI_TRIAL_WINDOW) return 0;
  return (WIFI_TRIAL_WINDOW - elapsed) / 1000;
}

const WifiConfig& netLiveConfig() { return live; }

bool netStaConnected() { return live.staEnabled && WiFi.status() == WL_CONNECTED; }

String netStaAddress() {
  return netStaConnected() ? WiFi.localIP().toString() : String("");
}

String netApAddress() { return WiFi.softAPIP().toString(); }
