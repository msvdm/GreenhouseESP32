#include "display.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include "config.h"
#include "control.h"
#include "sensors.h"

static Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

static bool chromeDrawn = false;
static float lastDrawnAvg = -999.0f;
static String networkLine = "";

// Row layout, kept in one place so the fillRect/setCursor pairs cannot drift.
#define ROW_NET      5
#define ROW_AVG     20
#define ROW_HEATER  55
#define ROW_HOURS   68
#define ROW_FAN     83
#define ROW_LEFT    98
#define ROW_RIGHT  110
#define SCREEN_W   160

static void clearRow(int y, int h) {
  tft.fillRect(0, y, SCREEN_W, h, ST77XX_BLACK);
}

void displayBegin() {
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
}

void displaySplash(int numLeft, int numRight, bool heaterDetected) {
  tft.setCursor(0, 0);
  tft.printf("Greenhouse v%s\n", FIRMWARE_VERSION);
  tft.printf("Sensors: L%d R%d H%d\n", numLeft, numRight, heaterDetected ? 1 : 0);
}

void displayNetwork(const String& ip) {
  networkLine = ip;

  // During startup this is still part of the splash, printed at the cursor
  // the splash left behind.
  if (!chromeDrawn) {
    tft.printf("IP: %s\n", ip.c_str());
    return;
  }

  // Called again later - a WiFi change applied at runtime. Printing here
  // would land wherever the cursor happens to be, so mark the chrome dirty
  // and let the normal repaint own the screen.
  chromeDrawn = false;
}

void updateDisplay() {
  if (!chromeDrawn) {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_GREEN);
    tft.setCursor(5, ROW_NET);
    tft.printf("IP: %s", networkLine.c_str());
    chromeDrawn = true;
    lastDrawnAvg = -999.0f;
  }

  const float avg = sensorData.averageTemp;

  // Average temperature - only repainted when it actually moves, because a
  // full-width repaint at 0.5 Hz is visibly flickery.
  if (fabsf(avg - lastDrawnAvg) > 0.1f) {
    clearRow(ROW_AVG, 30);
    tft.setCursor(1, ROW_AVG);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_YELLOW);
    tft.print(F("Avg:"));
    if (TEMP_IS_VALID(avg)) {
      if (avg < settings.tempMin)      tft.setTextColor(ST77XX_CYAN);
      else if (avg > settings.tempMax) tft.setTextColor(ST77XX_ORANGE);
      else                             tft.setTextColor(ST77XX_YELLOW);
      tft.printf("%.1fC", avg);
    } else {
      tft.setTextColor(ST77XX_RED);
      tft.print(F("ERR"));
    }
    lastDrawnAvg = avg;
  }

  tft.setTextSize(1);

  clearRow(ROW_HEATER, 12);
  tft.setCursor(2, ROW_HEATER);
  tft.setTextColor(ST77XX_RED);
  if (TEMP_IS_VALID(sensorData.heaterTemp)) {
    tft.printf("Heater:%.1fC %s", sensorData.heaterTemp, heaterOn ? "ON" : "OFF");
  } else {
    tft.print(F("Heater:ERR"));
  }

  clearRow(ROW_HOURS, 12);
  tft.setCursor(2, ROW_HOURS);
  tft.setTextColor(ST77XX_RED);
  tft.printf("Heating:%luh", stats.totalHeaterRuntime / 3600000UL);

  clearRow(ROW_FAN, 12);
  tft.setCursor(2, ROW_FAN);
  tft.setTextColor(ST77XX_GREEN);
  tft.printf("Fan: %s  ", fansOn ? "ON" : "OFF");
  tft.setTextColor(activeFault ? ST77XX_RED : ST77XX_YELLOW);
  tft.print(modeName(currentMode));

  // If a simulated value is armed, the little screen and the web page are
  // reporting different temperatures. That has to be visible from the doorway,
  // not only to whoever is holding the phone.
  if (sim.airActive || sim.heaterActive) {
    tft.setTextColor(ST77XX_MAGENTA);
    tft.print(F(" SIM"));
  }

  clearRow(ROW_LEFT, 24);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(2, ROW_LEFT);
  for (int i = 0; i < sensorData.numLeft; i++) {
    if (TEMP_IS_VALID(sensorData.left[i])) tft.printf("%.1f ", sensorData.left[i]);
    else                                   tft.print(F("-- "));
  }
  tft.setCursor(2, ROW_RIGHT);
  for (int i = 0; i < sensorData.numRight; i++) {
    if (TEMP_IS_VALID(sensorData.right[i])) tft.printf("%.1f ", sensorData.right[i]);
    else                                    tft.print(F("-- "));
  }
}

void displayOtaBegin() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(5, 20);
  tft.print(F("FIRMWARE UPDATE"));
  tft.setCursor(5, 35);
  tft.setTextColor(ST77XX_RED);
  tft.print(F("DO NOT POWER OFF"));

  // The control loop stops running during an update, so the normal repaint
  // path will not restore this screen - mark the chrome dirty for afterwards.
  chromeDrawn = false;
}

void displayOtaProgress(size_t done, size_t total) {
  static int lastShown = -1;

  tft.setTextSize(1);
  tft.fillRect(5, 60, SCREEN_W - 10, 12, ST77XX_BLACK);
  tft.setCursor(5, 60);
  tft.setTextColor(ST77XX_WHITE);

  if (total == 0) {
    // Browser upload: no Content-Length to divide by, so report raw progress.
    const int kb = (int)(done / 1024);
    if (kb == lastShown) return;
    lastShown = kb;
    tft.printf("%d KB received", kb);
    return;
  }

  const int percent = (int)((done * 100) / total);
  if (percent == lastShown) return;
  lastShown = percent;

  tft.printf("%d%%", percent);
  tft.drawRect(5, 78, SCREEN_W - 10, 10, ST77XX_WHITE);
  tft.fillRect(6, 79, ((SCREEN_W - 12) * percent) / 100, 8, ST77XX_GREEN);
}

void displayOtaResult(bool ok, const char* message) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ok ? ST77XX_GREEN : ST77XX_RED);
  tft.setCursor(5, 40);
  tft.print(ok ? F("UPDATE OK") : F("UPDATE FAILED"));
  tft.setCursor(5, 55);
  tft.setTextColor(ST77XX_WHITE);
  tft.print(message);
  chromeDrawn = false;
}

// ----------------------------------------------------------------------------
// FACTORY RESET
// ----------------------------------------------------------------------------
void displayHoldReset(int secondsLeft) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(5, 20);
  tft.print(F("FACTORY RESET"));

  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(5, 40);
  tft.print(F("Release to cancel"));

  tft.setTextSize(3);
  tft.setTextColor(ST77XX_RED);
  tft.setCursor(70, 65);
  tft.print(secondsLeft);
  tft.setTextSize(1);

  chromeDrawn = false;
}

void displayResetDone(const char* apSsid, const char* uiPass) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_GREEN);
  tft.setCursor(5, 30);
  tft.print(F("RESET TO DEFAULTS"));
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(5, 50);
  tft.printf("AP:  %s", apSsid);
  tft.setCursor(5, 62);
  tft.printf("Web: %s", uiPass);
  chromeDrawn = false;
}

void displayInvalidate() { chromeDrawn = false; }
