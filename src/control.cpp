#include "control.h"
#include "sensors.h"

SystemMode currentMode = MODE_IDLE;
Fault activeFault = FAULT_NONE;
bool heaterOn = false;
bool fansOn = false;

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
  }
  return "?";
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

  if (p.ventilate && residualHeat) {
    setFans(true, now);
    fanCooldownStart = now;
    currentMode = MODE_COOLDOWN;
  } else {
    setFans(false, now);
    currentMode = MODE_IDLE;
  }

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
  const float avg = sensorData.averageTemp;

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
  if (sensorData.heaterTemp <= 0.0f ||
      sensorData.heaterTemp >= settings.heaterMax - HEATER_ARM_MARGIN) {
    return;
  }

  setHeater(true, now);
  Serial.printf("Heater ON: avg=%.1fC heater=%.1fC\n",
                sensorData.averageTemp, sensorData.heaterTemp);
}

static void driveCooling(unsigned long now) {
  setHeater(false, now);
  if (!fansOn) {
    setFans(true, now);
    stats.totalCoolingCycles++;
    Serial.printf("COOLING: %.1fC\n", sensorData.averageTemp);
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
    case MODE_IDLE:     driveIdle(now);     break;
  }
}

// ============================================================================
// FAST SAFETY TICK (1 Hz)
// ============================================================================
void safetyTick(unsigned long now) {
  if (!TEMP_IS_VALID(sensorData.heaterTemp)) {
    // The element's temperature cannot be verified. Shed it here at 1 Hz
    // rather than waiting up to 5 s for the next full control pass.
    if (heaterOn) {
      Serial.println(F("Heater zone reading invalid - shedding element"));
      applyMode(MODE_COOLDOWN, now);
    }
    // Escalate to a named sensor fault once the grace period has expired.
    if (heaterSensorFailed()) enterFault(FAULT_HEATER_SENSOR_LOST, now);
    return;
  }

  if (sensorData.heaterTemp >= settings.heaterCritical) {
    Serial.printf("CRITICAL: heater zone %.1fC exceeds fan rated ambient\n",
                  sensorData.heaterTemp);
    enterFault(FAULT_HEATER_CRITICAL, now);
    return;
  }

  if (sensorData.heaterTemp >= settings.heaterMax && heaterOn) {
    // Over the element limit but below the fan's: shed the heater and purge.
    // Routed through applyMode so runtime accounting and the minimum-off-time
    // interlock are applied here exactly as on every other shutdown path.
    Serial.printf("Heater zone %.1fC at safety limit - forcing cooldown\n",
                  sensorData.heaterTemp);
    stats.heaterSafetyEvents++;
    applyMode(MODE_COOLDOWN, now);
  }
}

// ============================================================================
// MAIN CONTROL PASS (0.2 Hz)
// ============================================================================
void controlSystem(unsigned long now) {
  // Gate: the controller may not operate on inputs it cannot trust.
  if (!sensorData.heaterDetected || heaterSensorFailed()) {
    enterFault(FAULT_HEATER_SENSOR_LOST, now);
    return;
  }
  if (airSensorsFailed()) {
    enterFault(FAULT_AIR_SENSORS_LOST, now);
    return;
  }
  if (!TEMP_IS_VALID(sensorData.averageTemp) || !TEMP_IS_VALID(sensorData.heaterTemp)) {
    enterFault(FAULT_INVALID_READINGS, now);
    return;
  }

  // Inputs are valid and the heater zone is within limits: release any latched
  // fault so a genuinely new event is counted and logged again.
  if (sensorData.heaterTemp < settings.heaterMax) activeFault = FAULT_NONE;

  // Hard interlock: maximum continuous runtime. Routed through applyMode so it
  // gets the same cooldown entry handling as every other path into COOLDOWN.
  if (heaterOn && (now - heaterStartTime >= MAX_HEATER_RUNTIME)) {
    Serial.println(F("Max heater runtime reached - forcing cooldown"));
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
  Serial.printf("Average air : %.1fC\n", sensorData.averageTemp);
  Serial.printf("Heater zone : %.1fC\n", sensorData.heaterTemp);
  Serial.printf("Mode        : %s%s\n", modeName(currentMode),
                activeFault ? "  [FAULT]" : "");
  Serial.printf("Heat cycles : %lu\n", stats.totalHeatingCycles);
  Serial.printf("Runtime     : %luh\n", stats.totalHeaterRuntime / 3600000UL);
  Serial.printf("Temp range  : %.1fC - %.1fC\n",
                stats.minTempRecorded, stats.maxTempRecorded);
  Serial.println(F("---------------------------------------------------\n"));
}
