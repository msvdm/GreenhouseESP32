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
  MODE_MANUAL,     // operator drives the outputs; only the critical trip applies
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

// Human-readable fault, for the web UI. A latched fault outranks manual mode
// entirely - the control pass never reaches the mode machine - so without this
// the page would show a stale mode and no reason for it.
const char* faultName(Fault f);

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

// ============================================================================
// MANUAL MODE
// ============================================================================
// settings.manualMode is the operator's INTENT and is persisted. MODE_MANUAL
// is what the machine is doing right now, and is not. The two differ whenever
// a fault is active: enterFault() drives the mode to IDLE or COOLDOWN, and
// once the condition clears decideMode() returns to MODE_MANUAL because the
// intent never went away.
//
// This is not the parallel-flag mistake that inCooldownMode was. That was two
// names for one fact. Intent and current state are two different facts, in the
// same way a setpoint and a temperature are.
//
// The output REQUESTS below are deliberately not persisted, and neither are
// the simulated values. Manual mode survives a reboot; a latched heater relay
// must not.
extern bool manualHeaterReq;
extern bool manualFanReq;

//
// Manual is a TEST mode and the protections are stripped to match. The heater
// and fan relays are independent switches: no airflow proving, no heater-zone
// arm margin, no heaterMax shed, no thirty-minute runtime cap, no sensor fault
// gates. A manual heat run has no time limit at all while the zone probe reads.
//
// The single protection that survives is the critical trip at
// settings.heaterCritical, and it needs a readable probe to fire. Heating with
// no probe is therefore allowed but bounded by MANUAL_BLIND_HEAT_LIMIT - see
// safetyTick(), which owns that clock.
void setManualMode(bool on, unsigned long now);
void setManualHeater(bool on, unsigned long now);
void setManualFan(bool on, unsigned long now);

// Why a manual heater request is not currently energising the element. Empty
// when there is nothing holding it off. Shown on the web page so a blocked
// request looks blocked rather than broken.
const char* manualHoldReason();

// Seconds before an unsupervised manual heat run is shed. Zero whenever the
// zone probe is readable - so any non-zero value means the element is running
// with nothing but this countdown limiting it.
unsigned long manualBlindSecondsLeft();

// ============================================================================
// SIMULATED SENSORS
// ============================================================================
// A test aid, with one hard rule enforced in control.cpp: simulation may only
// ever make the controller MORE cautious, never less.
//
//   Air temperature   - overridden freely. Air is not a safety input; it
//                       decides whether to want heat, and every heater
//                       protection sits downstream of that decision.
//   Heater zone       - overridden UPWARDS ONLY, via max() against the real
//                       reading, and only while the real sensor is valid.
//                       Simulating the element hotter than it is trips the
//                       protections early, which is exactly what you want to
//                       test. Simulating it cooler would mask a genuinely hot
//                       element and silently delete the critical trip.
//
// Simulation is deliberately available in automatic mode as well as manual:
// watching the mode machine decide it wants heat is most of the point, and it
// cannot do that while manual mode is holding the machine still. Leaving manual
// therefore does NOT clear an armed override - SIM_TIMEOUT is the one rule that
// ends it, so there is only one rule to remember.
struct SimOverride {
  bool  airActive    = false;
  float air          = 15.0f;
  bool  heaterActive = false;
  float heater       = 30.0f;
  unsigned long armedAt = 0;
};

extern SimOverride sim;

bool setSimAir(bool on, float value);
bool setSimHeater(bool on, float value);
void clearSim();
