#include "sensors.h"
#include <OneWire.h>
#include <DallasTemperature.h>

SensorData sensorData;

static OneWire leftBus(LEFT_SENSORS_BUS);
static OneWire rightBus(RIGHT_SENSORS_BUS);
static OneWire heaterBus(HEATER_SENSOR_BUS);

static DallasTemperature leftSensors(&leftBus);
static DallasTemperature rightSensors(&rightBus);
static DallasTemperature heaterSensor(&heaterBus);

static DeviceAddress leftAddresses[MAX_SENSORS_PER_SIDE];
static DeviceAddress rightAddresses[MAX_SENSORS_PER_SIDE];
static DeviceAddress heaterAddress;

// Heater-zone validation state
static float lastValidHeaterTemp = TEMP_INVALID;
static unsigned long lastHeaterTempUpdate = 0;
static int heaterConsecutiveFailures = 0;
static unsigned long heaterFailureStart = 0;
static unsigned long airFailureStart = 0;

// A DS18B20 spike faster than this is thermally impossible and is rejected.
#define MAX_HEATER_SLEW_C 30.0f
// Number of bad reads tolerated before the reading is declared invalid.
#define HEATER_FAILURE_TOLERANCE 3

// ----------------------------------------------------------------------------
// Bring one bus up and report what it found. Replaces three near-identical
// copies of this block that differed only in their log strings.
// ----------------------------------------------------------------------------
static int initBus(DallasTemperature& bus, DeviceAddress* addresses,
                   int maxDevices, const char* label, int pin) {
  Serial.printf("\n[%s sensors, GPIO %d]\n", label, pin);

  bus.begin();
  bus.setWaitForConversion(false);

  const int found = min((int)bus.getDeviceCount(), maxDevices);
  Serial.printf("  Found: %d\n", found);

  for (int i = 0; i < found; i++) {
    if (bus.getAddress(addresses[i], i)) {
      bus.setResolution(addresses[i], 12);
      Serial.printf("  OK %s%d initialised\n", label, i);
    }
  }
  return found;
}

void sensorsBegin() {
  sensorData.numLeft =
      initBus(leftSensors, leftAddresses, MAX_SENSORS_PER_SIDE, "LEFT", LEFT_SENSORS_BUS);
  sensorData.numRight =
      initBus(rightSensors, rightAddresses, MAX_SENSORS_PER_SIDE, "RIGHT", RIGHT_SENSORS_BUS);

  const int heaterCount = initBus(heaterSensor, &heaterAddress, 1, "HEATER", HEATER_SENSOR_BUS);
  sensorData.heaterDetected = (heaterCount == 1);
  if (!sensorData.heaterDetected) {
    Serial.println(F("  *** HEATER SENSOR MISSING - HEATING DISABLED ***"));
  }

  // Start the first conversion on every bus now, so the first read in loop()
  // collects a completed measurement rather than the 85C power-on value.
  leftSensors.requestTemperatures();
  rightSensors.requestTemperatures();
  heaterSensor.requestTemperatures();
}

// ----------------------------------------------------------------------------
// HEATER ZONE - safety critical
// ----------------------------------------------------------------------------
// Returns true if the raw reading should be rejected, logging why.
static bool heaterReadingIsInvalid(float temp) {
  if (temp == DEVICE_DISCONNECTED_C || temp == TEMP_INVALID) {
    Serial.println(F("Heater sensor disconnected"));
    return true;
  }
  if (temp == 85.0f) {
    // 85.0 is the DS18B20 power-on scratchpad value, i.e. a sensor that reset.
    Serial.println(F("Heater sensor reported power-on value (85C) - rejected"));
    return true;
  }
  if (temp < -50.0f || temp > 100.0f) {
    Serial.printf("Heater sensor out of range: %.1fC\n", temp);
    return true;
  }
  if (TEMP_IS_VALID(lastValidHeaterTemp) &&
      (millis() - lastHeaterTempUpdate) < 60000UL &&
      fabsf(temp - lastValidHeaterTemp) > MAX_HEATER_SLEW_C) {
    Serial.printf("Heater sensor slew rejected: %.1fC -> %.1fC\n",
                  lastValidHeaterTemp, temp);
    return true;
  }
  return false;
}

void readHeaterTemperature() {
  if (!sensorData.heaterDetected) {
    sensorData.heaterTemp = TEMP_INVALID;
    return;
  }

  const float temp = heaterSensor.getTempC(heaterAddress);
  heaterSensor.requestTemperatures();   // pipeline the next conversion

  if (heaterReadingIsInvalid(temp)) {
    heaterConsecutiveFailures++;

    // Ride out brief glitches on the last known good value.
    if (heaterConsecutiveFailures < HEATER_FAILURE_TOLERANCE) {
      Serial.printf("  holding last valid %.1fC (failure %d/%d)\n",
                    lastValidHeaterTemp, heaterConsecutiveFailures,
                    HEATER_FAILURE_TOLERANCE);
      return;
    }

    sensorData.heaterTemp = TEMP_INVALID;
    if (heaterFailureStart == 0) {
      heaterFailureStart = millis();
      Serial.println(F("Heater sensor grace period started (30s)"));
    }
    return;
  }

  heaterConsecutiveFailures = 0;
  heaterFailureStart = 0;
  lastValidHeaterTemp = temp;
  lastHeaterTempUpdate = millis();
  sensorData.heaterTemp = temp;
}

bool heaterSensorFailed() {
  if (!sensorData.heaterDetected) return true;
  if (heaterFailureStart == 0) return false;
  return (millis() - heaterFailureStart) >= SENSOR_FAILURE_GRACE_PERIOD;
}

// ----------------------------------------------------------------------------
// AIR TEMPERATURES
// ----------------------------------------------------------------------------
static void readSide(DallasTemperature& bus, DeviceAddress* addresses,
                     float* out, int count) {
  for (int i = 0; i < count; i++) {
    const float temp = bus.getTempC(addresses[i]);
    const bool ok = (temp != DEVICE_DISCONNECTED_C && temp > -50.0f && temp < 60.0f);
    out[i] = ok ? temp : TEMP_INVALID;
  }
  bus.requestTemperatures();   // pipeline the next conversion
}

void readAirTemperatures() {
  readSide(leftSensors, leftAddresses, sensorData.left, sensorData.numLeft);
  readSide(rightSensors, rightAddresses, sensorData.right, sensorData.numRight);

  float sum = 0.0f;
  int validCount = 0;
  for (int i = 0; i < sensorData.numLeft; i++) {
    if (TEMP_IS_VALID(sensorData.left[i]))  { sum += sensorData.left[i];  validCount++; }
  }
  for (int i = 0; i < sensorData.numRight; i++) {
    if (TEMP_IS_VALID(sensorData.right[i])) { sum += sensorData.right[i]; validCount++; }
  }

  if (validCount == 0) {
    sensorData.averageTemp = TEMP_INVALID;
    if (airFailureStart == 0) {
      airFailureStart = millis();
      Serial.println(F("No valid air sensors - grace period started (30s)"));
    }
    return;
  }

  sensorData.averageTemp = sum / validCount;
  airFailureStart = 0;

  if (sensorData.averageTemp < stats.minTempRecorded) stats.minTempRecorded = sensorData.averageTemp;
  if (sensorData.averageTemp > stats.maxTempRecorded) stats.maxTempRecorded = sensorData.averageTemp;
}

bool airSensorsFailed() {
  if (airFailureStart == 0) return false;
  return (millis() - airFailureStart) >= SENSOR_FAILURE_GRACE_PERIOD;
}
