#include "display.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include "config.h"
#include "control.h"
#include "net.h"
#include "sensors.h"
#include "settings.h"

#define W 160
#define H 128

// RGB565
#define C_BG      0x0000
#define C_BAR     0x2104
#define C_TEXT    0xFFFF
#define C_DIM     0x8410
#define C_GREEN   0x07E0
#define C_YELLOW  0xFFE0
#define C_CYAN    0x07FF
#define C_ORANGE  0xFD20
#define C_RED     0xF800
#define C_HEAT    0xC800
#define C_FAN     0x033F

static Adafruit_ST7735 tft(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
static GFXcanvas16* canvas = nullptr;

static void push() {
  tft.drawRGBBitmap(0, 0, canvas->getBuffer(), W, H);
}

static void centered(const char* s, int y, uint8_t size, uint16_t color) {
  canvas->setTextSize(size);
  canvas->setTextColor(color);
  canvas->setCursor((W - (int)strlen(s) * 6 * size) / 2, y);
  canvas->print(s);
}

void displayBegin() {
  // Must come before initR(): the library's own SPI.begin() is then a no-op,
  // so the bus keeps these pins instead of the VSPI defaults.
  SPI.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, -1);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(C_BG);
  canvas = new GFXcanvas16(W, H);
  canvas->cp437(true);   // char 248 = degree sign
  canvas->setTextWrap(false);
  displayMessage("GREENHOUSE", "v" FW_VERSION, "starting...", C_GREEN);
}

// 74x43 box: name, big ON/OFF, and why (max 10 characters).
static void relayBox(int x, const char* title, bool on, uint16_t onColor, const char* detail) {
  const int y = 57, w = 74, h = 43;
  if (on) canvas->fillRoundRect(x, y, w, h, 5, onColor);
  else    canvas->drawRoundRect(x, y, w, h, 5, C_DIM);

  canvas->setTextColor(on ? C_TEXT : C_DIM);
  canvas->setTextSize(1);
  canvas->setCursor(x + 6, y + 4);
  canvas->print(title);
  canvas->setTextSize(2);
  canvas->setCursor(x + 6, y + 15);
  canvas->print(on ? "ON" : "OFF");
  canvas->setTextSize(1);
  canvas->setCursor(x + 6, y + 33);
  canvas->print(detail);
}

void displayUpdate() {
  if (!canvas) return;
  canvas->fillScreen(C_BG);
  char buf[32];

  // Web address
  canvas->fillRect(0, 0, W, 13, C_BAR);
  canvas->setTextSize(1);
  if (netStaConnected()) {
    canvas->setTextColor(C_GREEN);
    snprintf(buf, sizeof(buf), "http://%s", netStaIp().c_str());
  } else {
    canvas->setTextColor(C_YELLOW);
    snprintf(buf, sizeof(buf), "AP http://%s", netApIp().c_str());
  }
  canvas->setCursor(4, 3);
  canvas->print(buf);

  // Average temperature
  const float avg = sensorsAverage();
  if (isnan(avg)) {
    centered("--.-", 18, 3, C_RED);
    centered("NO SENSOR", 46, 1, C_RED);
  } else {
    uint16_t color = C_TEXT;
    if (avg <= settings.heatOn)      color = C_CYAN;
    else if (avg >= settings.coolOn) color = C_ORANGE;
    snprintf(buf, sizeof(buf), "%.1f%cC", avg, (char)248);
    centered(buf, 18, 3, color);

    // Per line: average and probe count
    canvas->setTextSize(1);
    for (int i = 0; i < 2; i++) {
      const SensorLine& l = sensorLines[i];
      if (isnan(l.avg)) snprintf(buf, sizeof(buf), "L%d --.- (%d)", i + 1, l.count);
      else              snprintf(buf, sizeof(buf), "L%d %.1f (%d)", i + 1, l.avg, l.count);
      canvas->setTextColor(isnan(l.avg) ? C_RED : C_DIM);
      canvas->setCursor(i == 0 ? 4 : 84, 46);
      canvas->print(buf);
    }
  }

  // Relays
  const char* heatDetail = ctrl.heaterMode == RELAY_AUTO ? "auto" : "manual";
  const char* fr = fanReason();
  const char* fanDetail = ctrl.fanMode != RELAY_AUTO ? "manual"
                        : (fr[0] == 'c' ? "cooling" : fr[0] == 'r' ? "room heat" : "auto");
  relayBox(4, "HEATING", ctrl.heater, C_HEAT, heatDetail);
  relayBox(82, "FANS", ctrl.fan, C_FAN, fanDetail);

  // Setpoints and heating mode
  canvas->setTextSize(1);
  canvas->setTextColor(C_TEXT);
  canvas->setCursor(4, 104);
  canvas->printf("Heat<=%.1f  Fan>=%.1f", settings.heatOn, settings.coolOn);
  canvas->setCursor(4, 116);
  canvas->setTextColor(settings.roomHeating ? C_ORANGE : C_DIM);
  canvas->print(settings.roomHeating ? "Room heating (+fans)" : "Zone heating");

  push();
}

void displayMessage(const char* title, const char* line1, const char* line2, uint16_t color) {
  if (!canvas) return;
  canvas->fillScreen(C_BG);
  centered(title, 30, 2, color);
  centered(line1, 64, 1, C_TEXT);
  centered(line2, 80, 1, C_TEXT);
  push();
}

void displayOta(const char* msg, int percent) {
  if (!canvas) return;
  static int lastPercent = -2;
  if (percent == lastPercent && percent >= 0) return;   // only repaint on change
  lastPercent = percent;

  canvas->fillScreen(C_BG);
  centered("FIRMWARE UPDATE", 20, 1, C_YELLOW);
  centered(msg, 40, 1, C_TEXT);
  centered("do not power off", 112, 1, C_RED);
  if (percent >= 0) {
    canvas->drawRect(10, 64, W - 20, 14, C_TEXT);
    canvas->fillRect(12, 66, (W - 24) * percent / 100, 10, C_GREEN);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", percent);
    centered(buf, 86, 1, C_TEXT);
  }
  push();
}
