#pragma once
#include <Arduino.h>
#include "config.h"

// ============================================================================
// NETWORK - access point, optional station, and the trial/commit machinery
// ============================================================================
// The board ALWAYS hosts its own access point. Enabling the station side adds
// a second interface (WIFI_AP_STA); it never replaces the AP. That is what
// makes the settings on this page safe to change from a phone standing in the
// greenhouse: whatever you get wrong, the access point is still there, and so
// is the OTA upload port.
//
// THE TRIAL COVERS THE ACCESS POINT ONLY, and that is the whole design here.
// Changing the AP - its name, its password, its address - can lock you out of
// the board, so it follows the bargain a router offers: the new settings are
// applied to the radio but NOT written to flash, and you have
// WIFI_TRIAL_WINDOW to reconnect and confirm. Miss it and the board puts the
// previous configuration back by itself. Because the candidate lives only in
// RAM, a reboot during the trial also comes up on the committed configuration,
// so no sequence of events can leave unconfirmed credentials in flash.
//
// Station changes get none of that, because they cannot lock you out of
// anything: the AP is untouched, so a wrong password costs you a home-network
// address you did not have a moment ago. Making the operator confirm those was
// pure friction, and it cost them their access point every time they tried.

void netBegin();
void netLoop(unsigned long now);

// Apply the ACCESS POINT fields of a candidate on trial. The station side of
// the candidate is ignored. Applied a few hundred milliseconds later so the
// HTTP reply reaches the browser before the radio drops it. Returns false if
// the candidate could not be made valid.
bool netApplyAp(const WifiConfig& candidate);

// Apply the STATION fields immediately and commit them. No trial: nothing here
// can cost you access to the board. Returns false on an invalid candidate.
bool netApplySta(const WifiConfig& candidate);

// Queue the factory configuration, on trial - it resets the AP too.
bool netFactoryReset();

// Keep the configuration currently on the radio. Returns false if no trial is
// running (a confirmation that arrives after the revert, for instance).
bool netConfirm();

// Put the committed configuration back now rather than waiting out the window.
bool netRevert();

bool netTrialActive();
unsigned long netTrialSecondsLeft();

// What the radio is running right now - the candidate during a trial.
const WifiConfig& netLiveConfig();

bool netStaConnected();
String netStaAddress();
String netApAddress();

// The mDNS name currently announced, without the .local suffix. Empty when
// mDNS is switched off. ota.cpp takes its ArduinoOTA hostname from here.
const char* netHostname();

// ============================================================================
// SSID SCAN
// ============================================================================
// ALWAYS ASYNCHRONOUS. A blocking WiFi.scanNetworks() parks loop() for two to
// four seconds, which stalls the 1 Hz heater-zone read and safetyTick() with
// it. The 20 s task watchdog would not catch that - it would simply be four
// seconds of a 2200 W element running unsupervised. There is no version of
// this feature worth that, so the scan is started here and collected later.
void netStartScan();

// WIFI_SCAN_RUNNING (-1) while a scan is in flight, WIFI_SCAN_FAILED (-2) when
// there are no results to read - which is also the idle state, before any scan
// has been started - and >= 0 for the number of networks found. Results are
// read straight from WiFi.SSID(i) / RSSI(i) / encryptionType(i).
int netScanState();

// ============================================================================
// ASSOCIATED CLIENTS
// ============================================================================
// The ESP32 SoftAP tops out at four associated stations.
#define AP_CLIENT_MAX 4

struct ApClient {
  char mac[18];   // "aa:bb:cc:dd:ee:ff"
  char ip[16];
};

// Fills out[] with up to max clients and returns how many were written.
int netApClients(ApClient* out, int max);
