#pragma once
#include <Arduino.h>

// ============================================================================
// TFT STATUS DISPLAY (ST7735, 160x128)
// ============================================================================
void displayBegin();

// Shown once during startup, before the network is up.
void displaySplash(int numLeft, int numRight, bool heaterDetected);
void displayNetwork(const String& ip);

void updateDisplay();

// Full-screen takeover while firmware is being written.
void displayOtaBegin();
// total == 0 means the transfer size is unknown (browser upload), in which
// case bytes received are shown instead of a percentage.
void displayOtaProgress(size_t done, size_t total);
void displayOtaResult(bool ok, const char* message);
