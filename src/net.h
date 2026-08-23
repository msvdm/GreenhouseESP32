#pragma once
#include <Arduino.h>
#include "config.h"

// ============================================================================
// NETWORK - access point, optional station, and the trial/commit machinery
// ============================================================================
// The board ALWAYS hosts its own access point. Enabling the station side adds
// a second interface (WIFI_AP_STA); it never replaces the AP. That is what
// makes the settings on this page safe to change from a phone standing in the
// greenhouse: whatever you get wrong, 192.168.4.1 is still there, and so is
// the OTA upload port.
//
// Changing the configuration follows the bargain a router offers. The new
// settings are applied to the radio but NOT written to flash. You have
// WIFI_TRIAL_WINDOW to reconnect and confirm. Miss it, and the board puts the
// previous configuration back by itself. Because the candidate lives only in
// RAM, a reboot during the trial also comes up on the committed configuration -
// no sequence of events can leave unconfirmed credentials in flash.

void netBegin();
void netLoop(unsigned long now);

// Queue a candidate configuration. Applied a few hundred milliseconds later so
// the HTTP reply reaches the browser before the radio drops it. Returns false
// if the candidate could not be made valid.
bool netRequestTrial(const WifiConfig& candidate);

// Queue the factory configuration, on trial like any other change.
bool netRequestFactoryReset();

// Keep the configuration currently on the radio. Returns false if no trial is
// running (a confirmation that arrives after the revert, for instance).
bool netConfirm();

bool netTrialActive();
unsigned long netTrialSecondsLeft();

// What the radio is running right now - the candidate during a trial.
const WifiConfig& netLiveConfig();

bool netStaConnected();
String netStaAddress();
String netApAddress();
