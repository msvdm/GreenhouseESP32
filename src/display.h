#pragma once
#include <Arduino.h>

// ============================================================================
// ST7735 160x128 STATUS SCREEN
// ============================================================================
// Every frame is drawn into an off-screen canvas and pushed in one go, so
// nothing flickers and there is no partial-redraw bookkeeping.

void displayBegin();
void displayUpdate();                                  // normal status screen
void displayMessage(const char* title, const char* line1, const char* line2, uint16_t color);
void displayOta(const char* msg, int percent);         // percent < 0 = unknown
