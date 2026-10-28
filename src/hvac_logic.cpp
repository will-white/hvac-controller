#include "hvac_logic.h"
#include <Arduino.h>
#include <cmath>
#include <algorithm>
#include "config.h"
#include "main.h"
#include "relays.h"
#include "health.h"
#include "sensors.h"
#include "utils.h"

// -- Global State Variables for this module --
unsigned long lastHeaterOnTime = 0, lastHeaterOffTime = 0;
unsigned long lastCoolerOnTime = 0, lastCoolerOffTime = 0;
// Circulate accounting: actual fan runtime (any cause) accrues toward the
// per-hour quota, so heat/cool cycles reduce what circulate has to add.
unsigned long circHourStartTime = 0;
unsigned long fanOnMsThisHour = 0;
unsigned long lastFanAccountTime = 0;
// Which demand the fresh-air damper is currently serving. Without this, the
// AUTO-mode branch gates can't tell their own economizer session from the
// opposite branch's, and the damper latches open forever.
enum FreshAirPurpose { FA_NONE, FA_HEATING, FA_COOLING };
FreshAirPurpose freshAirPurpose = FA_NONE;
float cycleStartTempF = 0;
float cycleStartOutdoorTempF = 0;
unsigned long cycleStartTime = 0;
int heaterMaxRunTriggers = 0;
unsigned long firstHeaterMaxRunTriggerTime = 0;
int coolerMaxRunTriggers = 0;
unsigned long firstCoolerMaxRunTriggerTime = 0;

// Convert minutes to milliseconds for internal use
const unsigned long MIN_HEATER_RUN_TIME_MS = MIN_HEATER_RUN_TIME_MINS * 60 * 1000;
const unsigned long MIN_HEATER_OFF_TIME_MS = MIN_HEATER_OFF_TIME_MINS * 60 * 1000;
const unsigned long MAX_HEATER_RUN_TIME_MS = MAX_HEATER_RUN_TIME_MINS * 60 * 1000;
const unsigned long MIN_COOLER_RUN_TIME_MS = MIN_COOLER_RUN_TIME_MINS * 60 * 1000;
const unsigned long MIN_COOLER_OFF_TIME_MS = MIN_COOLER_OFF_TIME_MINS * 60 * 1000;
const unsigned long MAX_COOLER_RUN_TIME_MS = MAX_COOLER_RUN_TIME_MINS * 60 * 1000;
const unsigned long FAN_CIRCULATE_TARGET_MS = FAN_CIRCULATE_MINS_PER_HOUR * 60 * 1000;
const unsigned long FAN_OFF_DELAY_AFTER_HEAT_MS = FAN_OFF_DELAY_AFTER_HEAT_SECS * 1000;
const unsigned long MAX_RUN_LOCKOUT_MS = MAX_RUN_LOCKOUT_HOURS * 60 * 60 * 1000;

void initialize_logic_timers() {
  unsigned long now = currentTime();
  // The heater may start immediately after boot...
  lastHeaterOffTime = now - (MIN_HEATER_OFF_TIME_MS + 1000);
  // ...but the compressor must wait out a full min-off period from power-up:
  // after a power blip it can still be pressure-equalized against a hard start.
  lastCoolerOffTime = now;
  circHourStartTime = now;
  lastFanAccountTime = now;
  fanOnMsThisHour = 0;
  freshAirPurpose = FA_NONE;
}

int getPerformanceBin(float tempF) {
    if (tempF < 10) return 0; if (tempF < 20) return 1;
    if (tempF < 30) return 2; if (tempF < 40) return 3;
    if (tempF < 50) return 4; if (tempF < 60) return 5;
    if (tempF < 70) return 6; return 7;
}

void updatePerformanceData(bool isHeating) {
    if (cycleStartTime == 0) return;
    unsigned long durationMs = currentTime() - cycleStartTime;
    unsigned long minRunMs = isHeating ? MIN_HEATER_RUN_TIME_MS : MIN_COOLER_RUN_TIME_MS;
    if (durationMs < (minRunMs - 1000)) return;

    // readTemperature() returns the configured display unit; learning data is stored in F.
    float rawTemp = readTemperature();
    float currentTempF = (tempUnit == FAHRENHEIT) ? rawTemp : celsiusToFahrenheit(rawTemp);
    float tempChangeF = fabsf(currentTempF - cycleStartTempF);
    float durationHours = (float)durationMs / (1000.0 * 60.0 * 60.0);
    float rateF_PerHour = tempChangeF / durationHours;

    int bin = getPerformanceBin(cycleStartOutdoorTempF);
    int season = getCurrentSeason();

    if (isHeating) {
        int oldCount = performance.heatSamples[season][bin];
        health_reportCycleRate(true, rateF_PerHour, performance.heatRate[season][bin], oldCount);
        performance.heatRate[season][bin] = ((performance.heatRate[season][bin] * oldCount) + rateF_PerHour) / (oldCount + 1);
        performance.heatSamples[season][bin]++;
    } else {
        int oldCount = performance.coolSamples[season][bin];
        health_reportCycleRate(false, rateF_PerHour, performance.coolRate[season][bin], oldCount);
        performance.coolRate[season][bin] = ((performance.coolRate[season][bin] * oldCount) + rateF_PerHour) / (oldCount + 1);
        performance.coolSamples[season][bin]++;
    }
    performanceDirty = true; // flushed to NVS on a timer, not per-cycle (flash wear)
    cycleStartTime = 0;
}

void resetLockout() {
  systemLockedOut = false;
  heaterMaxRunTriggers = 0;
  coolerMaxRunTriggers = 0;
}

// Cut every output with the same off-time bookkeeping a normal cycle end does,
// so min-off protection (compressor!) still holds when control resumes after a
// fault or a gap in indoor readings. Writes unconditionally every call: that
// also re-asserts mask 0 on the MOD-IO2, which holds whatever it was last told.
void stopAllEquipment() {
  unsigned long now = currentTime();
  if (readRelay(HEATER_RELAY)) lastHeaterOffTime = now;
  if (readRelay(COOLER_RELAY)) lastCoolerOffTime = now;
  writeRelay(HEATER_RELAY, LOW);
  writeRelay(COOLER_RELAY, LOW);
  writeRelay(FAN_RELAY, LOW);
  writeRelay(FRESH_AIR_RELAY, LOW);
  writeRelay(HUMIDITY_RELAY, LOW);
  freshAirPurpose = FA_NONE;
  cycleStartTime = 0; // an interrupted cycle is not a performance sample
}

bool coolerMinOffElapsed() {
  return currentTime() - lastCoolerOffTime > MIN_COOLER_OFF_TIME_MS;
}

bool getActiveCycleInfo(bool& isHeating, unsigned long& startTime, float& startTempF) {
  bool h = readRelay(HEATER_RELAY), c = readRelay(COOLER_RELAY);
  if ((!h && !c) || cycleStartTime == 0) return false;
  isHeating = h;
  startTime = cycleStartTime;
  startTempF = cycleStartTempF;
  return true;
}

void controlTemperature(float tempC, float heatTargetC, float coolTargetC, float outdoorTempC, SystemMode mode) {
  if (systemLockedOut) {
    // Record the off-times: after `reset lockout` the min-off timers must
    // still protect the equipment.
    if(readRelay(HEATER_RELAY)) { writeRelay(HEATER_RELAY, LOW); lastHeaterOffTime = currentTime(); }
    if(readRelay(COOLER_RELAY)) { writeRelay(COOLER_RELAY, LOW); lastCoolerOffTime = currentTime(); }
    if(readRelay(FRESH_AIR_RELAY)) writeRelay(FRESH_AIR_RELAY, LOW);
    freshAirPurpose = FA_NONE;
    return;
  }

  unsigned long now = currentTime();
  bool isHeatingOn = readRelay(HEATER_RELAY);
  bool isCoolingOn = readRelay(COOLER_RELAY);
  bool isFreshAirOn = readRelay(FRESH_AIR_RELAY);
  if (!isFreshAirOn) freshAirPurpose = FA_NONE; // resync if damper was closed elsewhere
  bool freshAirHeating = isFreshAirOn && freshAirPurpose == FA_HEATING;
  bool freshAirCooling = isFreshAirOn && freshAirPurpose == FA_COOLING;

  float tempDeadbandC = (tempUnit == FAHRENHEIT) ? fahrenheitToCelsius(32.0 + TEMPERATURE_DEADBAND_F) - fahrenheitToCelsius(32.0) : TEMPERATURE_DEADBAND_F;
  float freshAirDiffC = (tempUnit == FAHRENHEIT) ? fahrenheitToCelsius(32.0 + FRESH_AIR_TEMP_DIFFERENTIAL_F) - fahrenheitToCelsius(32.0) : FRESH_AIR_TEMP_DIFFERENTIAL_F;

  // In AUTO, each branch defers only to the OTHER branch's activity. (Gating on
  // the damper alone deadlocks: with it open, neither branch could run, so
  // nothing could ever close it.)
  if (mode == SYS_HEAT || (mode == SYS_AUTO && !isCoolingOn && !freshAirCooling)) {
    if (isHeatingOn && (now - lastHeaterOnTime > MAX_HEATER_RUN_TIME_MS)) {
        writeRelay(HEATER_RELAY, LOW); lastHeaterOffTime = now;
        cycleStartTime = 0; // abnormal cycle (fault/open window): don't feed it to the learner
        health_reportMaxRun(true);
        if (heaterMaxRunTriggers > 0 && (now - firstHeaterMaxRunTriggerTime > MAX_RUN_LOCKOUT_MS)) { heaterMaxRunTriggers = 1; firstHeaterMaxRunTriggerTime = now; }
        else { if (heaterMaxRunTriggers == 0) firstHeaterMaxRunTriggerTime = now; heaterMaxRunTriggers++; if (heaterMaxRunTriggers >= MAX_RUN_TRIGGER_COUNT) { systemLockedOut = true; saveSafetyState("heater hit max-run limit 3x"); } }
    } else if (tempC >= heatTargetC && (isHeatingOn || freshAirHeating)) {
      if (isHeatingOn && (now - lastHeaterOnTime > MIN_HEATER_RUN_TIME_MS)) { writeRelay(HEATER_RELAY, LOW); lastHeaterOffTime = now; updatePerformanceData(true); }
      else if (freshAirHeating) { writeRelay(FRESH_AIR_RELAY, LOW); freshAirPurpose = FA_NONE; }
    } else if (tempC < (heatTargetC - tempDeadbandC) && !isHeatingOn) {
      bool economizerViable = outdoorTempC > (tempC + freshAirDiffC);
      // Free heating that is still losing ground gets escalated to the furnace.
      bool economizerLosing = freshAirHeating && tempC < (heatTargetC - 2 * tempDeadbandC);
      if (economizerViable && !economizerLosing) {
        if (!isFreshAirOn) { writeRelay(FRESH_AIR_RELAY, HIGH); freshAirPurpose = FA_HEATING; }
      } else {
        // Mechanical heat takes over: the damper must not stay open behind it.
        if (isFreshAirOn) { writeRelay(FRESH_AIR_RELAY, LOW); freshAirPurpose = FA_NONE; }
        if (now - lastHeaterOffTime > MIN_HEATER_OFF_TIME_MS && !readRelay(COOLER_RELAY)) {
          writeRelay(HEATER_RELAY, HIGH); lastHeaterOnTime = now;
          cycleStartTime = now; cycleStartTempF = celsiusToFahrenheit(tempC); cycleStartOutdoorTempF = celsiusToFahrenheit(outdoorTempC);
          health_reportCycleStart(true);
        }
      }
    }
  }

  if (mode == SYS_COOL || (mode == SYS_AUTO && !isHeatingOn && !freshAirHeating)) {
    if (isCoolingOn && (now - lastCoolerOnTime > MAX_COOLER_RUN_TIME_MS)) {
        writeRelay(COOLER_RELAY, LOW); lastCoolerOffTime = now;
        cycleStartTime = 0; // abnormal cycle (fault/open window): don't feed it to the learner
        health_reportMaxRun(false);
        if (coolerMaxRunTriggers > 0 && (now - firstCoolerMaxRunTriggerTime > MAX_RUN_LOCKOUT_MS)) { coolerMaxRunTriggers = 1; firstCoolerMaxRunTriggerTime = now; }
        else { if (coolerMaxRunTriggers == 0) firstCoolerMaxRunTriggerTime = now; coolerMaxRunTriggers++; if (coolerMaxRunTriggers >= MAX_RUN_TRIGGER_COUNT) { systemLockedOut = true; saveSafetyState("cooler hit max-run limit 3x"); } }
    } else if (tempC <= coolTargetC && (isCoolingOn || freshAirCooling)) {
      if (isCoolingOn && (now - lastCoolerOnTime > MIN_COOLER_RUN_TIME_MS)) { writeRelay(COOLER_RELAY, LOW); lastCoolerOffTime = now; updatePerformanceData(false); }
      else if (freshAirCooling) { writeRelay(FRESH_AIR_RELAY, LOW); freshAirPurpose = FA_NONE; }
    } else if (tempC > (coolTargetC + tempDeadbandC) && !isCoolingOn) {
      bool economizerViable = outdoorTempC < (tempC - freshAirDiffC);
      // Free cooling that is still losing ground gets escalated to the compressor.
      bool economizerLosing = freshAirCooling && tempC > (coolTargetC + 2 * tempDeadbandC);
      if (economizerViable && !economizerLosing) {
        if (!isFreshAirOn) { writeRelay(FRESH_AIR_RELAY, HIGH); freshAirPurpose = FA_COOLING; }
      } else {
        // Mechanical cooling takes over: the damper must not stay open behind it.
        if (isFreshAirOn) { writeRelay(FRESH_AIR_RELAY, LOW); freshAirPurpose = FA_NONE; }
        if (now - lastCoolerOffTime > MIN_COOLER_OFF_TIME_MS && !readRelay(HEATER_RELAY)) {
          writeRelay(COOLER_RELAY, HIGH); lastCoolerOnTime = now;
          cycleStartTime = now; cycleStartTempF = celsiusToFahrenheit(tempC); cycleStartOutdoorTempF = celsiusToFahrenheit(outdoorTempC);
          health_reportCycleStart(false);
        }
      }
    }
  }
}

void controlFan(FanMode fanMode) {
  unsigned long now = currentTime();

  // Account actual fan runtime (any cause) toward the circulate quota.
  if (now - circHourStartTime >= 3600000UL) { circHourStartTime = now; fanOnMsThisHour = 0; }
  if (readRelay(FAN_RELAY)) {
    unsigned long d = now - lastFanAccountTime;
    if (d < 3600000UL) fanOnMsThisHour += d;
  }
  lastFanAccountTime = now;

  bool equipmentActive = (readRelay(HEATER_RELAY) || readRelay(COOLER_RELAY) || readRelay(FRESH_AIR_RELAY));
  // Post-heat purge: keep moving air briefly to harvest exchanger warmth.
  bool heatPurge = !readRelay(HEATER_RELAY) && lastHeaterOnTime != 0 &&
                   (now - lastHeaterOffTime < FAN_OFF_DELAY_AFTER_HEAT_MS);

  if (equipmentActive || heatPurge) { if (!readRelay(FAN_RELAY)) { writeRelay(FAN_RELAY, HIGH); } return; }

  switch(fanMode) {
    case FAN_ON: if (!readRelay(FAN_RELAY)) { writeRelay(FAN_RELAY, HIGH); } break;
    case FAN_CIRCULATE: {
      // Deadline scheduling: start the fan when the remaining hour just fits
      // the unmet quota; once started, run until the quota is met.
      unsigned long deficit = (FAN_CIRCULATE_TARGET_MS > fanOnMsThisHour) ? FAN_CIRCULATE_TARGET_MS - fanOnMsThisHour : 0;
      unsigned long hourRemaining = 3600000UL - (now - circHourStartTime);
      bool fanOn = readRelay(FAN_RELAY);
      if (deficit == 0) { if (fanOn) writeRelay(FAN_RELAY, LOW); }
      else if (fanOn || hourRemaining <= deficit + CONTROL_INTERVAL_MS) { if (!fanOn) writeRelay(FAN_RELAY, HIGH); }
      break;
    }
    case FAN_AUTO: default: if (readRelay(FAN_RELAY)) { writeRelay(FAN_RELAY, LOW); } break;
  }
}

float calculateMaxHumidityForWindow(float indoorTempC, float outdoorTempC) {
  float estimatedWindowTemp = outdoorTempC + WINDOW_EFFICIENCY_FACTOR * (indoorTempC - outdoorTempC);
  float targetDewPoint = estimatedWindowTemp - 2.0;
  double a = 17.625, b = 243.04;
  double alpha_td = (a * targetDewPoint) / (b + targetDewPoint);
  double alpha_t = (a * indoorTempC) / (b + indoorTempC);
  double maxRH = 100.0 * exp(alpha_td - alpha_t);
  if (maxRH > 95.0) return 95.0; if (maxRH < 10.0) return 10.0;
  return (float)maxRH;
}

void controlHumidity(float humidity, float indoorTempC, float outdoorTempC) {
  bool isHumidityControlOn = readRelay(HUMIDITY_RELAY);

  // Bypass/duct humidifiers need airflow to move moisture, and humidifying
  // while the A/C dehumidifies is self-defeating.
  if ((HUMIDIFIER_REQUIRES_AIRFLOW && !readRelay(FAN_RELAY)) || readRelay(COOLER_RELAY)) {
    if (isHumidityControlOn) writeRelay(HUMIDITY_RELAY, LOW);
    return;
  }

  float maxHumidityLimit = calculateMaxHumidityForWindow(indoorTempC, outdoorTempC);
  float effectiveTarget = std::min(HUMIDITY_TARGET, maxHumidityLimit);

  if (humidity < (effectiveTarget - HUMIDITY_DEADBAND) && !isHumidityControlOn) { writeRelay(HUMIDITY_RELAY, HIGH); } 
  else if (humidity >= effectiveTarget && isHumidityControlOn) { writeRelay(HUMIDITY_RELAY, LOW); }
}