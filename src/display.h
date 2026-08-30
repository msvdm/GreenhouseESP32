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

// ============================================================================
// FACTORY RESET (IO0 BUTTON)
// ============================================================================
// The carrier board's status LED sits on the TFT's MOSI line, so it flickers
// with SPI traffic and cannot indicate anything. The screen is the only
// feedback channel this board has, which is what makes the countdown matter:
// without it a held button is indistinguishable from a dead one.
void displayHoldReset(int secondsLeft);

// The credentials are passed in rather than printed from literals here, so the
// screen cannot end up announcing values that secrets.h no longer sets.
void displayResetDone(const char* apSsid, const char* uiPass);

// Marks the screen dirty so the next updateDisplay() repaints it in full.
// Used when a takeover screen ends without a reboot behind it - a factory-reset
// countdown the operator let go of, for instance.
void displayInvalidate();
