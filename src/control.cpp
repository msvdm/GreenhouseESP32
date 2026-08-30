#include "control.h"
#include "sensors.h"

SystemMode currentMode = MODE_IDLE;
Fault activeFault = FAULT_NONE;
bool heaterOn = false;
bool fansOn = false;

bool manualHeaterReq = false;
bool manualFanReq = false;
SimOverride sim;
static const char* manualHold = "";

static unsigned long heaterStartTime = 0;
static unsigned long lastHeaterOffTime = 0;
static unsigned long fanStartTime = 0;
static unsigned long fanCooldownStart = 0;

// When the manual heater started running with no readable zone probe. Zero
// whenever there is a real reading, or whenever the element is off.
static unsigned long blindSince = 0;

// Manual taps reach the relays through here rather than waiting for the next
// control pass, so this has to be declared before the setters that call it.
static void driveManual(unsigned long now);

#define CYCLE_HISTORY_SIZE 10
static unsigned long heatingCycleTimestamps[CYCLE_HISTORY_SIZE] = { 0 };
static int cycleHistoryIndex = 0;

const char* modeName(SystemMode m) {
  switch (m) {
    case MODE_IDLE:     return "IDLE";
    case MODE_HEATING:  return "HEATING";
    case MODE_COOLING:  return "COOLING";
    case MODE_COOLDOWN: return "COOLDOWN";
    case MODE_MANUAL:   return "MANUAL";
  }
  return "?";
}

// ============================================================================
// SENSOR VALUES AS THE CONTROL LOGIC SEES THEM
// ============================================================================
// The asymmetry between these two functions is the whole safety argument for
// the simulation feature, so it is spelled out rather than left to be noticed.

// Air is not a safety input. It answers "should we want heat?", and every
// protection that matters sits downstream of that answer, so an operator may
// override it in either direction.
static float airTempForControl() {
  return sim.airActive ? sim.air : sensorData.averageTemp;
}

// The heater zone IS the safety input, so its override is one-directional:
// max() means a simulated value can only ever report the element HOTTER than
// it really is. That trips the protections early - harmless, and exactly what
// you want to be able to test. The reverse would mask a hot element and
// silently remove the critical trip, which is the single worst thing this
// feature could have been allowed to do.
//
// And if the real sensor is not readable the override does not apply at all: a
// failed heater sensor must still fault, whatever the operator typed in.
static float heaterTempForControl() {
  const float real = sensorData.heaterTemp;
  if (!sim.heaterActive || !TEMP_IS_VALID(real)) return real;
  return max(real, sim.heater);
}

// ============================================================================
// ACTUATORS
// ============================================================================
// These two functions are the ONLY code permitted to touch the relay pins.
// Routing every change through them is what makes the runtime accounting and
// the minimum-off-time interlock impossible for a caller to forget - which is
// exactly how those two used to get skipped on the emergency paths.
static void setHeater(bool on, unsigned long now) {
  if (on == heaterOn) return;

  digitalWrite(HEATER_RELAY_PIN, on ? RELAY_ON : RELAY_OFF);
  heaterOn = on;

  if (on) {
    heaterStartTime = now;
    heatingCycleTimestamps[cycleHistoryIndex] = now;
    cycleHistoryIndex = (cycleHistoryIndex + 1) % CYCLE_HISTORY_SIZE;
    stats.totalHeatingCycles++;
  } else {
    stats.totalHeaterRuntime += (now - heaterStartTime);
    lastHeaterOffTime = now;
  }
}

static void setFans(bool on, unsigned long now) {
  if (on == fansOn) return;

  digitalWrite(FAN_RELAY_PIN, on ? RELAY_ON : RELAY_OFF);
  fansOn = on;

  // Stamped on every off->on transition, so "the fans have been running for at
  // least FAN_STARTUP_DELAY" is a fact rather than a hopeful assumption.
  if (on) fanStartTime = now;
}

void controlBegin() {
  // Drive the safe level first, then enable the drivers, then reassert - so a
  // pin cannot glitch a relay closed while it is changing direction.
  digitalWrite(HEATER_RELAY_PIN, RELAY_OFF);
  digitalWrite(FAN_RELAY_PIN, RELAY_OFF);
  pinMode(HEATER_RELAY_PIN, OUTPUT);
  pinMode(FAN_RELAY_PIN, OUTPUT);
  digitalWrite(HEATER_RELAY_PIN, RELAY_OFF);
  digitalWrite(FAN_RELAY_PIN, RELAY_OFF);

  heaterOn = false;
  fansOn = false;
  Serial.println(F("Safety: all outputs OFF"));
}

// ============================================================================
// MANUAL MODE
// ============================================================================
// A safety path has taken the element away from the operator. Drop the manual
// heater request so it cannot silently re-arm the instant the condition
// clears - a shutdown nobody asked for should need a deliberate press to
// undo - and set the fan request to whatever that path decided, because
// driveManual() would otherwise override it on the very next pass. That last
// part matters most on the critical trip, which deliberately does NOT
// ventilate: leaving the fan request standing would restart the fan past its
// own rated ambient.
static void manualSafetyShed(bool ventilate) {
  if (!settings.manualMode) return;
  manualHeaterReq = false;
  manualFanReq = ventilate;
}

void setManualMode(bool on, unsigned long now) {
  if (on == settings.manualMode) return;
  settings.manualMode = on;

  if (on) {
    // Seed the fan request from the actual output so switching to manual in
    // the middle of a purge does not cut the purge short. The heater always
    // starts from OFF: entering manual must never be a route to keeping an
    // element energised.
    manualFanReq = fansOn;
    manualHeaterReq = false;

    // The latch is deliberately NOT cleared here. Clearing it made every
    // manual round trip re-raise the same condition on the way back to
    // automatic, and each of those counted as a fresh safety shutdown - the
    // exact statistics inflation the transition guard in enterFault() exists
    // to prevent. Manual ignores these faults instead of erasing them:
    // driveManual() refuses only on the critical trip, and the web UI blocks
    // the heater tap only on that same one.
  } else {
    manualHeaterReq = false;
    manualFanReq = false;
    setHeater(false, now);

    // Hand back to the automatic machine with a purge if there is residual
    // heat. setHeater() has just stamped lastHeaterOffTime, so the five-minute
    // rest that manual mode bypasses applies again from this moment.
    if (fansOn) {
      currentMode = MODE_COOLDOWN;
      fanCooldownStart = now;
    } else {
      currentMode = MODE_IDLE;
    }
  }

  manualHold = "";

  // Both directions. A stale timestamp left over from a previous manual session
  // would make (now - blindSince) already exceed the limit, so the first blind
  // heat request after re-entering manual would be shed on the spot - a
  // protection firing against the wrong run.
  blindSince = 0;

  settingsChangedTime = (now == 0) ? 1 : now;   // debounced write, handled by loop()
  Serial.printf("Control mode: %s\n", on ? "MANUAL" : "AUTOMATIC");
}

// Both apply to the relays immediately rather than waiting up to five seconds
// for the next control pass. A test switch that appears to do nothing for five
// seconds gets pressed again, and pressing a 2200 W heater switch twice because
// it looked broken is not a failure mode worth having.
//
// Each records the request and then runs driveManual() itself, so exactly one
// function decides what manual does with a relay. The earlier version made part
// of that decision here and part of it there, and the two had to be kept in
// step by hand.
void setManualHeater(bool on, unsigned long now) {
  if (!settings.manualMode) return;
  manualHeaterReq = on;
  driveManual(now);
}

void setManualFan(bool on, unsigned long now) {
  if (!settings.manualMode) return;
  manualFanReq = on;
  driveManual(now);
}

// Seconds before an unsupervised manual heat run is shed by safetyTick(). Zero
// whenever the zone probe is readable, so the web UI can treat any non-zero
// value as "the only protection left is a countdown".
unsigned long manualBlindSecondsLeft() {
  if (blindSince == 0) return 0;
  const unsigned long elapsed = millis() - blindSince;
  if (elapsed >= MANUAL_BLIND_HEAT_LIMIT) return 0;
  return (MANUAL_BLIND_HEAT_LIMIT - elapsed) / 1000;
}

const char* manualHoldReason() { return manualHold; }

// ============================================================================
// SIMULATED SENSORS
// ============================================================================
static bool simValueOk(float v) { return v >= SIM_TEMP_MIN && v <= SIM_TEMP_MAX; }

bool setSimAir(bool on, float value) {
  if (on && !simValueOk(value)) return false;
  sim.airActive = on;
  if (on) { sim.air = value; sim.armedAt = millis(); }
  return true;
}

bool setSimHeater(bool on, float value) {
  if (on && !simValueOk(value)) return false;
  sim.heaterActive = on;
  if (on) { sim.heater = value; sim.armedAt = millis(); }
  return true;
}

void clearSim() { sim = SimOverride(); }

// Simulation is a test aid, and a test aid that outlives the test is a hazard.
// Manual mode is allowed to be sticky because the operator can hear the relays
// and see the light-grey page; an override quietly replacing the greenhouse
// temperature is invisible, so it gets an expiry instead.
static void expireSim(unsigned long now) {
  if (!sim.airActive && !sim.heaterActive) return;
  if ((now - sim.armedAt) < SIM_TIMEOUT) return;
  clearSim();
  Serial.println(F("Simulation expired - real sensor readings restored"));
}

// ============================================================================
// FAULT HANDLING
// ============================================================================
// Each fault names its own response. The distinction that matters is
// ventilate: when the heater zone reading is missing or suspect the element
// may still be hot, so we purge it. The one case where we do NOT ventilate is
// the critical trip, because heaterCritical is the FAN's own rated ambient -
// past that point running the fan destroys the fan without protecting
// anything, and the element has already been shed.
struct FaultPolicy {
  const char* name;
  unsigned long SystemStats::* counter;
  bool ventilate;
};

static const FaultPolicy FAULT_POLICY[] = {
  { "none",                  nullptr,                              false },
  { "Heater sensor failure", &SystemStats::heaterSensorFailures,   true  },
  { "Heater critical temp",  &SystemStats::heaterCriticalEvents,   false },
  { "Air sensors failed",    &SystemStats::airSensorFailures,      true  },
  { "Invalid sensor data",   &SystemStats::invalidReadingEvents,   true  },
};

const char* faultName(Fault f) { return FAULT_POLICY[f].name; }

static void enterFault(Fault f, unsigned long now) {
  // Decided BEFORE the heater is shed, because shedding it stamps
  // lastHeaterOffTime: is there residual heat that actually needs purging?
  // On a cold boot with nothing wired the answer is no, and closing the fan
  // relay to purge an element that has never run is merely surprising.
  //
  // "The heater has run at some point this boot" is NOT that question, and
  // testing it that way was a defect: lastHeaterOffTime stays set for the rest
  // of the boot, so every later fault re-opened a purge for an element that
  // went cold long ago. Combined with the latched-fault case below it meant a
  // single manual heat run left the fan on for good, and switching to manual
  // and back started it again every time. A full purge length since the
  // element was last off is exactly the point at which there is nothing left
  // to purge.
  const bool residualHeat = heaterOn ||
      (lastHeaterOffTime != 0 && (now - lastHeaterOffTime) < FAN_COOLDOWN_TIME);

  // The heater comes off on every call, including re-assertions.
  setHeater(false, now);

  // Only the transition into a fault is counted and logged; a condition that
  // persists across read cycles must not inflate the statistics.
  if (activeFault == f) {
    // A purge is a fixed length, and it has to end even though the fault has
    // not. The gate in controlSystem() returns here on every pass while a
    // sensor fault is latched, so decideMode() - which owns the normal
    // COOLDOWN timeout - is never reached to end it. For a missing probe the
    // condition never clears at all, so without this the fan ran forever.
    if (currentMode == MODE_COOLDOWN && (now - fanCooldownStart) >= FAN_COOLDOWN_TIME) {
      setFans(false, now);
      currentMode = MODE_IDLE;
      Serial.println(F("Purge complete - fans off (fault still latched)"));
    }
    return;
  }
  activeFault = f;

  const FaultPolicy& p = FAULT_POLICY[f];
  stats.safetyShutdownCount++;
  if (p.counter) (stats.*(p.counter))++;

  const bool ventilate = p.ventilate && residualHeat;
  if (ventilate) {
    setFans(true, now);
    fanCooldownStart = now;
    currentMode = MODE_COOLDOWN;
  } else {
    setFans(false, now);
    currentMode = MODE_IDLE;
  }
  manualSafetyShed(ventilate);

  Serial.println(F("\n=== SAFETY EVENT - AUTO-RECOVERY ==="));
  Serial.printf("Reason: %s\n", p.name);
  Serial.printf("Fans:   %s\n", fansOn ? "RUNNING (purging heater zone)"
                                            : "OFF (nothing to purge / past fan rating)");
  Serial.println(F("System will auto-resume when readings are valid again.\n"));
}

void forceSafeState(bool ventilate) {
  const unsigned long now = millis();
  setHeater(false, now);
  setFans(ventilate, now);
  currentMode = ventilate ? MODE_COOLDOWN : MODE_IDLE;
  if (ventilate) fanCooldownStart = now;
  manualSafetyShed(ventilate);
}

// ============================================================================
// CYCLE LIMIT (sliding one-hour window)
// ============================================================================
static bool heatingCycleLimitOk(unsigned long now) {
  int cyclesInWindow = 0;
  for (unsigned long ts : heatingCycleTimestamps) {
    // Compared as an elapsed difference rather than against (now - 1h). The
    // latter underflows for the first hour after boot - silently disabling
    // this limit on every restart - and again at the millis() wrap.
    if (ts != 0 && (now - ts) < CYCLE_WINDOW) cyclesInWindow++;
  }
  return cyclesInWindow < MAX_HEATING_CYCLES_PER_HOUR;
}

// ============================================================================
// MODE SELECTION
// ============================================================================
// Where the controller wants to be, decided from temperature alone. No side
// effects: every actuator change is made by applyMode() below, so the
// hysteresis rules can be read and changed without tracing relay writes.
static SystemMode decideMode(unsigned long now) {
  // Operator intent outranks the temperature band entirely.
  if (settings.manualMode) return MODE_MANUAL;

  const float avg = airTempForControl();

  if (currentMode == MODE_COOLDOWN) {
    // An over-temperature during the purge outranks finishing the purge.
    if (avg > settings.tempMax + MODE_CHANGE_HYSTERESIS) return MODE_COOLING;
    if (now - fanCooldownStart >= FAN_COOLDOWN_TIME) return MODE_IDLE;
    return MODE_COOLDOWN;
  }

  if (avg < settings.tempMin) return MODE_HEATING;
  if (avg > settings.tempMax) return MODE_COOLING;

  // Heating deliberately overshoots tempMin to avoid short cycling.
  if (currentMode == MODE_HEATING &&
      avg >= settings.tempMin + HEATING_TARGET_OFFSET + HYSTERESIS) {
    return MODE_COOLDOWN;
  }
  if (currentMode == MODE_COOLING && avg <= settings.tempMax - HYSTERESIS) {
    return MODE_IDLE;
  }

  return currentMode;   // inside the deadband: hold
}

// ============================================================================
// MODE EXECUTION
// ============================================================================
static void driveHeating(unsigned long now) {
  if (heaterOn) return;   // already running - nothing left to arm

  // Interlock 1: minimum rest between heating cycles.
  if (lastHeaterOffTime > 0 && (now - lastHeaterOffTime) < MIN_HEATER_OFF_TIME) {
    Serial.printf("Minimum off-time: %lus remaining\n",
                  (MIN_HEATER_OFF_TIME - (now - lastHeaterOffTime)) / 1000);
    return;
  }

  // Interlock 2: cycles-per-hour cap.
  if (!heatingCycleLimitOk(now)) {
    Serial.println(F("Heating cycle limit reached - waiting"));
    return;
  }

  // Interlock 3: airflow must be established before the element energises.
  setFans(true, now);
  if (now - fanStartTime < FAN_STARTUP_DELAY) {
    Serial.printf("Fan startup: %lus remaining\n",
                  (FAN_STARTUP_DELAY - (now - fanStartTime)) / 1000);
    return;
  }

  // Interlock 4: heater zone must be plausibly cool, with margin.
  const float heat = heaterTempForControl();
  if (heat <= 0.0f || heat >= settings.heaterMax - HEATER_ARM_MARGIN) {
    return;
  }

  setHeater(true, now);
  Serial.printf("Heater ON: avg=%.1fC heater=%.1fC\n", airTempForControl(), heat);
}

// Manual execution. Manual is a TEST mode and is now treated as one: the two
// relays are independent switches and every zone protection is deliberately
// absent. Gone from here are the airflow-proving delay, the requirement for a
// valid zone reading and the heaterMax arm margin; gone from controlSystem()
// are the sensor fault gates and the thirty-minute runtime cap; gone from
// safetyTick() is the heaterMax shed. The fan no longer follows the heater
// either - testing the relays freely means the element can be energised with
// nothing moving air over it, and that is the operator's call to make.
//
// What is left is the critical trip in safetyTick(), and the clock that runs
// when there is no probe for that trip to fire against. That is the whole of it.
//
// None of this stops the bypassed limits being RECORDED. setHeater() still
// stamps lastHeaterOffTime and the cycle history, so returning to automatic
// re-applies the five-minute rest and the cycles-per-hour cap immediately,
// counting every manual cycle as history.
static void driveManual(unsigned long now) {
  setFans(manualFanReq, now);          // independent of the heater, on purpose

  if (!manualHeaterReq) {
    setHeater(false, now);
    manualHold = "";
    return;
  }

  // The one refusal left. A latched critical trip must need a deliberate second
  // press to undo: without this, a stale tab reasserting its request would put
  // the element straight back on at the fan's own rated ambient.
  if (activeFault == FAULT_HEATER_CRITICAL) {
    manualHold = "critical trip";
    setHeater(false, now);
    return;
  }

  manualHold = "";
  if (!heaterOn) {
    Serial.printf("MANUAL heater ON: heater=%.1fC%s\n", sensorData.heaterTemp,
                  TEMP_IS_VALID(sensorData.heaterTemp) ? "" : "  [NO PROBE - UNSUPERVISED]");
  }
  setHeater(true, now);
}

static void driveCooling(unsigned long now) {
  setHeater(false, now);
  if (!fansOn) {
    setFans(true, now);
    stats.totalCoolingCycles++;
    Serial.printf("COOLING: %.1fC\n", airTempForControl());
  }
}

static void driveCooldown(unsigned long now) {
  setHeater(false, now);
  setFans(true, now);
}

static void driveIdle(unsigned long now) {
  setHeater(false, now);
  setFans(false, now);
}

static void applyMode(SystemMode target, unsigned long now) {
  if (target != currentMode) {
    Serial.printf("Mode change: %s -> %s\n", modeName(currentMode), modeName(target));
    if (target == MODE_COOLDOWN) fanCooldownStart = now;   // entry action
    currentMode = target;
  }

  switch (currentMode) {
    case MODE_HEATING:  driveHeating(now);  break;
    case MODE_COOLING:  driveCooling(now);  break;
    case MODE_COOLDOWN: driveCooldown(now); break;
    case MODE_MANUAL:   driveManual(now);   break;
    case MODE_IDLE:     driveIdle(now);     break;
  }
}

// ============================================================================
// FAST SAFETY TICK (1 Hz)
// ============================================================================
void safetyTick(unsigned long now) {
  const float heat = heaterTempForControl();

  if (!TEMP_IS_VALID(heat)) {
    // Manual runs blind on purpose - it is the only way to exercise the relays
    // with nothing wired - so there is no fault escalation and no forced purge
    // here. What there is instead is a clock: with no reading, the critical
    // trip below cannot fire, so for this whole window nothing at all is
    // limiting 2200 W. MANUAL_BLIND_HEAT_LIMIT is how long that is allowed to
    // last, and it is the only thing standing between a disconnected probe and
    // an element that runs until someone walks out to the greenhouse.
    if (settings.manualMode) {
      if (!heaterOn) { blindSince = 0; return; }
      if (blindSince == 0) {
        blindSince = now;
        Serial.println(F("MANUAL: heating with no zone reading - unsupervised clock started"));
      }
      // Elapsed difference, never (now - LIMIT): the latter underflows for the
      // first five minutes after boot and again at the millis() wrap, which
      // would silently disable exactly this.
      if ((now - blindSince) >= MANUAL_BLIND_HEAT_LIMIT) {
        Serial.println(F("MANUAL: unsupervised heat limit reached - shedding element"));
        manualHeaterReq = false;      // must be pressed again, deliberately
        setHeater(false, now);
        blindSince = 0;
      }
      return;
    }

    // The element's temperature cannot be verified. Shed it here at 1 Hz
    // rather than waiting up to 5 s for the next full control pass.
    if (heaterOn) {
      Serial.println(F("Heater zone reading invalid - shedding element"));
      manualSafetyShed(true);
      applyMode(MODE_COOLDOWN, now);
    }
    // Escalate to a named sensor fault once the grace period has expired.
    if (heaterSensorFailed()) enterFault(FAULT_HEATER_SENSOR_LOST, now);
    return;
  }

  // A real reading: the critical trip is live again, so the clock is moot.
  blindSince = 0;

  // The one protection manual keeps. Unchanged in either mode.
  if (heat >= settings.heaterCritical) {
    Serial.printf("CRITICAL: heater zone %.1fC exceeds fan rated ambient\n", heat);
    enterFault(FAULT_HEATER_CRITICAL, now);
    return;
  }

  // Automatic only. In manual the operator owns the element up to the critical
  // trip, which is the whole point of stripping this one.
  if (!settings.manualMode && heat >= settings.heaterMax && heaterOn) {
    // Over the element limit but below the fan's: shed the heater and purge.
    // Routed through applyMode so runtime accounting and the minimum-off-time
    // interlock are applied here exactly as on every other shutdown path.
    Serial.printf("Heater zone %.1fC at safety limit - forcing cooldown\n", heat);
    stats.heaterSafetyEvents++;
    manualSafetyShed(true);
    applyMode(MODE_COOLDOWN, now);
  }
}

// ============================================================================
// MAIN CONTROL PASS (0.2 Hz)
// ============================================================================
void controlSystem(unsigned long now) {
  // Before the gates, so an expiry still happens while a fault is latched.
  expireSim(now);

  // Automatic may not operate on inputs it cannot trust. Manual skips all three
  // deliberately: every one of them ends in enterFault(), which takes the relays
  // away from the operator who is standing there testing them. A missing sensor
  // is an expected bench condition, not an emergency, and the consequence of
  // ignoring it - heating with no supervision - is bounded by the clock in
  // safetyTick() instead.
  if (!settings.manualMode) {
    // The heater zone gates read sensorData directly - the real probe, no
    // override - so a simulated value can never satisfy one of them.
    if (!sensorData.heaterDetected || heaterSensorFailed()) {
      enterFault(FAULT_HEATER_SENSOR_LOST, now);
      return;
    }
    // A simulated AIR temperature does satisfy the air-sensor gate. That is the
    // point of it - exercising the mode machine on a bench with nothing but the
    // heater-zone probe wired - and it is safe because every heater protection
    // sits downstream of this and still reads the real sensor.
    if (!sim.airActive && airSensorsFailed()) {
      enterFault(FAULT_AIR_SENSORS_LOST, now);
      return;
    }
    if (!TEMP_IS_VALID(airTempForControl()) || !TEMP_IS_VALID(sensorData.heaterTemp)) {
      enterFault(FAULT_INVALID_READINGS, now);
      return;
    }
  }

  // Release a latched fault so a genuinely new event is counted and logged
  // again. This needs a REAL reading, not just a value below the limit: a
  // disconnected probe reports -127, which is below every threshold there is,
  // and would otherwise clear a critical trip by virtue of having failed.
  if (TEMP_IS_VALID(sensorData.heaterTemp) &&
      heaterTempForControl() < settings.heaterMax) {
    activeFault = FAULT_NONE;
  }

  // Hard interlock: maximum continuous runtime. Routed through applyMode so it
  // gets the same cooldown entry handling as every other path into COOLDOWN.
  // Automatic only - in manual this is one of the stripped protections, so a
  // manual heat run has no time limit at all while the probe is readable.
  if (!settings.manualMode && heaterOn &&
      (now - heaterStartTime >= MAX_HEATER_RUNTIME)) {
    Serial.println(F("Max heater runtime reached - forcing cooldown"));
    manualSafetyShed(true);
    applyMode(MODE_COOLDOWN, now);
    return;
  }

  applyMode(decideMode(now), now);
}

// ============================================================================
// PERIODIC LOG
// ============================================================================
void logStatistics() {
  Serial.println(F("\n---------------- SYSTEM STATISTICS ----------------"));
  Serial.printf("Average air : %.1fC%s\n", sensorData.averageTemp,
                sim.airActive ? "  [SIM ACTIVE]" : "");
  Serial.printf("Heater zone : %.1fC%s\n", sensorData.heaterTemp,
                sim.heaterActive ? "  [SIM ACTIVE]" : "");
  Serial.printf("Mode        : %s%s\n", modeName(currentMode),
                activeFault ? "  [FAULT]" : "");
  Serial.printf("Heat cycles : %lu\n", stats.totalHeatingCycles);
  Serial.printf("Runtime     : %luh\n", stats.totalHeaterRuntime / 3600000UL);
  Serial.printf("Temp range  : %.1fC - %.1fC\n",
                stats.minTempRecorded, stats.maxTempRecorded);
  Serial.println(F("---------------------------------------------------\n"));
}
