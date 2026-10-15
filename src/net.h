#pragma once
#include <Arduino.h>
#include "settings.h"

// ============================================================================
// NETWORK - own access point (always on) + optional home network, mDNS, espota
// ============================================================================
// The access point stays up permanently so the board can always be reached at
// 192.168.4.1, even when the home network is down or misconfigured.

void netBegin();
void netLoop();

const WifiSettings& netConfig();
// Save new settings; they take effect after the reboot the caller schedules.
void netSave(const WifiSettings& w);

bool netStaConnected();
String netStaIp();
String netApIp();
// The address to show people: the home-network IP if connected, else the AP IP.
String netDisplayAddress();

// Firmware update in progress (espota or browser).
bool otaActive();
void otaSetActive(bool active);
