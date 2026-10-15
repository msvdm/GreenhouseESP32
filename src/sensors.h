#pragma once
#include <Arduino.h>
#include "config.h"

// ============================================================================
// TEMPERATURE - two DS18B20 lines, any number of probes on each
// ============================================================================
// Invalid values are NAN.
//
//   line average = mean of the valid probes on that line
//   room average = mean of the line averages that are available
//
// So both lines weigh the same no matter how many probes each one carries.

struct SensorLine {
  uint8_t pin;
  uint8_t count = 0;                     // probes found on the last scan
  float temps[MAX_PROBES_PER_LINE];      // per probe, NAN if unreadable
  float avg = NAN;                       // NAN once stale (SENSOR_STALE)
  unsigned long lastValid = 0;
};

extern SensorLine sensorLines[2];

void sensorsBegin();
void sensorsUpdate(unsigned long now);   // call every SENSOR_INTERVAL
float sensorsAverage();                  // NAN if neither line has a reading
