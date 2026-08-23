#pragma once
#include <Arduino.h>
#include "config.h"

// ============================================================================
// CONTROL - actuators, fault policy and the mode machine
// ============================================================================
// This module owns every write to the relay pins and every decision about
// them. Nothing else in the firmware calls digitalWrite() on a relay.

enum SystemMode {
  MODE_IDLE,
  MODE_HEATING,
  MODE_COOLING,
  MODE_COOLDOWN,   // fan purge after heating; also the ventilating fault state
};

// Order must match FAULT_POLICY[] in control.cpp.
enum Fault {
  FAULT_NONE = 0,
  FAULT_HEATER_SENSOR_LOST,
  FAULT_HEATER_CRITICAL,
  FAULT_AIR_SENSORS_LOST,
  FAULT_INVALID_READINGS,
};

extern SystemMode currentMode;
extern Fault activeFault;
extern bool heaterOn;
extern bool fansOn;

const char* modeName(SystemMode m);

void controlBegin();

// 1 Hz. Only the trips that must not wait for the slower control pass: the
// critical over-temperature and the heater-zone limit.
void safetyTick(unsigned long now);

// 0.2 Hz. Full mode selection and execution.
void controlSystem(unsigned long now);

// Force everything to a safe state and hold it there. Used by the OTA path,
// where the CPU is about to stop servicing the control loop entirely.
void forceSafeState(bool ventilate);

void logStatistics();
