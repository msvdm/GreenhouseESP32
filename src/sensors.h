#pragma once
#include <Arduino.h>
#include "config.h"

// ============================================================================
// TEMPERATURE SENSING (DS18B20 on three independent OneWire buses)
// ============================================================================
// This module owns acquisition and validation only. It has no knowledge of
// faults, relays or modes - it reports what it can see, and control.cpp
// decides what that means. Invalid readings are reported as -127.0f.

#define TEMP_INVALID -127.0f
#define TEMP_IS_VALID(t) ((t) > -100.0f)

struct SensorData {
  float left[MAX_SENSORS_PER_SIDE]  = { TEMP_INVALID, TEMP_INVALID, TEMP_INVALID };
  float right[MAX_SENSORS_PER_SIDE] = { TEMP_INVALID, TEMP_INVALID, TEMP_INVALID };
  int numLeft = 0;
  int numRight = 0;
  float averageTemp = TEMP_INVALID;   // mean of every valid air sensor
  float heaterTemp = TEMP_INVALID;    // heater zone, safety critical
  bool heaterDetected = false;
};

extern SensorData sensorData;

void sensorsBegin();

// Both are conversion-pipelined: each call reads the conversion started by the
// previous call and immediately starts the next one. At 12-bit resolution a
// DS18B20 needs 750 ms, so reading straight after requesting - as this code
// used to - returned the previous value anyway, but without acknowledging it.
void readHeaterTemperature();
void readAirTemperatures();

// True once the respective sensors have been unreadable for longer than
// SENSOR_FAILURE_GRACE_PERIOD. Brief glitches do not count.
bool heaterSensorFailed();
bool airSensorsFailed();
