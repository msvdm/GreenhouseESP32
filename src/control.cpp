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
  settingsChangedTime = (now == 0) ? 1 : now;   // debounced write, handled by loop()
  Serial.printf("Control mode: %s\n", on ? "MANUAL" : "AUTOMATIC");
}

// Both apply to the relays immediately rather than waiting up to five seconds
// for the next control pass. A test switch that appears to do nothing for five
// seconds gets pressed again, and pressing a 2200 W heater switch twice
// because it looked broken is not a failure mode worth having.
void setManualHeater(bool on, unsigned long now) {
  if (!settings.manualMode) return;
  manualHeaterReq = on;

  // Switching ON only starts the fans - the element itself is left to
  // driveManual(), which is where the interlocks live. Timing the airflow
  // delay from the press rather than from the next pass just means the
  // operator waits five seconds instead of up to ten.
  if (on) setFans(true, now);
  else    setHeater(false, now);
}

void setManualFan(bool on, unsigned long now) {
  if (!settings.manualMode) return;
  manualFanReq = on;
  setFans(on || manualHeaterReq, now);   // the heater's requirement still wins
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

unsigned long simSecondsLeft() {
  if (!sim.airActive && !sim.heaterActive) return 0;
  const unsigned long elapsed = millis() - sim.armedAt;
  if (elapsed >= SIM_TIMEOUT) return 0;
  return (SIM_TIMEOUT - elapsed) / 1000;
}

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
  const bool residualHeat = heaterOn || (lastHeaterOffTime != 0);

  // The heater comes off on every call, including re-assertions.
  setHeater(false, now);

  // Only the transition into a fault is counted and logged; a condition that
  // persists across read cycles must not inflate the statistics.
  if (activeFault == f) return;
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

// Manual execution. Interlocks 1 and 2 - the five-minute rest and the
// cycles-per-hour cap - are deliberately bypassed here so the system can be
// exercised repeatedly during a test. Everything that protects the hardware
// rather than its duty cycle stays exactly as it is in automatic: airflow
// before heat, a valid heater-zone reading, the zone limit with its margin,
// and (from controlSystem) the thirty-minute runtime cap.
//
// Bypassing those two does not stop them being RECORDED. setHeater() still
// stamps lastHeaterOffTime and the cycle history, so returning to automatic
// applies both limits immediately, counting the manual cycles as history.
static void driveManual(unsigned long now) {
  // A heater request implies airflow whatever the fan toggle says.
  setFans(manualFanReq || manualHeaterReq, now);

  if (!manualHeaterReq) {
    setHeater(false, now);
    manualHold = "";
    return;
  }

  if (now - fanStartTime < FAN_STARTUP_DELAY) {
    manualHold = "proving airflow";
    return;
  }

  // The real sensor, through the upward-only override. No path here lets a
  // simulated value energise an element that should stay off.
  const float heat = heaterTempForControl();
  if (!TEMP_IS_VALID(heat) || heat <= 0.0f) {
    manualHold = "no heater zone reading";
    return;
  }
  if (heat >= settings.heaterMax - HEATER_ARM_MARGIN) {
    manualHold = "heater zone too hot";
    return;
  }

  manualHold = "";
  if (!heaterOn) Serial.printf("MANUAL heater ON: heater=%.1fC\n", heat);
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

  if (heat >= settings.heaterCritical) {
    Serial.printf("CRITICAL: heater zone %.1fC exceeds fan rated ambient\n", heat);
    enterFault(FAULT_HEATER_CRITICAL, now);
    return;
  }

  if (heat >= settings.heaterMax && heaterOn) {
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

  // Gate: the controller may not operate on inputs it cannot trust. The heater
  // zone gates read sensorData directly - the real probe, no override - so a
  // simulated value can never satisfy one of them.
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

  // Inputs are valid and the heater zone is within limits: release any latched
  // fault so a genuinely new event is counted and logged again.
  if (heaterTempForControl() < settings.heaterMax) activeFault = FAULT_NONE;

  // Hard interlock: maximum continuous runtime. Routed through applyMode so it
  // gets the same cooldown entry handling as every other path into COOLDOWN.
  // In manual it also drops the heater request: with the minimum rest
  // bypassed, leaving that request standing would re-energise the element on
  // the very next pass and make the cap meaningless.
  if (heaterOn && (now - heaterStartTime >= MAX_HEATER_RUNTIME)) {
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
