#include "sensors.h"
#include <DallasTemperature.h>
#include <OneWire.h>

SensorLine sensorLines[2];

static OneWire wire1(PIN_LINE_1);
static OneWire wire2(PIN_LINE_2);
static DallasTemperature bus[2] = { DallasTemperature(&wire1), DallasTemperature(&wire2) };
static DeviceAddress addr[2][MAX_PROBES_PER_LINE];
static unsigned long lastScan = 0;

// (Re)discover the probes on one line and start a conversion. Cheap enough to
// run every SENSOR_RESCAN, which is what makes probes hot-pluggable.
static void scanLine(int i) {
  SensorLine& line = sensorLines[i];
  bus[i].begin();
  bus[i].setWaitForConversion(false);

  uint8_t found = 0;
  const uint8_t n = bus[i].getDeviceCount();
  for (uint8_t k = 0; k < n && found < MAX_PROBES_PER_LINE; k++) {
    if (bus[i].getAddress(addr[i][found], k)) found++;
  }
  if (found != line.count) {
    Serial.printf("[sensors] line %d (GPIO %d): %d probe(s)\n", i + 1, line.pin, found);
  }
  line.count = found;
  bus[i].setResolution(12);
  for (uint8_t k = found; k < MAX_PROBES_PER_LINE; k++) line.temps[k] = NAN;
}

static bool plausible(float t) {
  // -127 = no answer / CRC error, 85.0 = power-on value of a probe that reset.
  return t != DEVICE_DISCONNECTED_C && t != 85.0f && t > -40.0f && t < 80.0f;
}

void sensorsBegin() {
  sensorLines[0].pin = PIN_LINE_1;
  sensorLines[1].pin = PIN_LINE_2;
  for (int i = 0; i < 2; i++) {
    for (float& t : sensorLines[i].temps) t = NAN;
    scanLine(i);
    bus[i].requestTemperatures();
  }
  lastScan = millis();
}

void sensorsUpdate(unsigned long now) {
  const bool rescan = (now - lastScan) >= SENSOR_RESCAN;
  if (rescan) lastScan = now;

  for (int i = 0; i < 2; i++) {
    SensorLine& line = sensorLines[i];

    // Collect the conversion started on the previous pass (>= 750 ms ago).
    float sum = 0.0f;
    int valid = 0;
    for (uint8_t k = 0; k < line.count; k++) {
      const float t = bus[i].getTempC(addr[i][k]);
      line.temps[k] = plausible(t) ? t : NAN;
      if (!isnan(line.temps[k])) { sum += line.temps[k]; valid++; }
    }

    if (valid > 0) {
      line.avg = sum / valid;
      line.lastValid = now;
    } else if (!isnan(line.avg) && (now - line.lastValid) >= SENSOR_STALE) {
      // Ride out short glitches on the last value, then give up.
      line.avg = NAN;
      Serial.printf("[sensors] line %d: no valid reading\n", i + 1);
    }

    if (rescan) scanLine(i);
    bus[i].requestTemperatures();
  }
}

float sensorsAverage() {
  float sum = 0.0f;
  int n = 0;
  for (const SensorLine& line : sensorLines) {
    if (!isnan(line.avg)) { sum += line.avg; n++; }
  }
  return n ? sum / n : NAN;
}
