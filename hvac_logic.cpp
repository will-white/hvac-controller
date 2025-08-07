#include "hvac_logic.h"
#include <Arduino.h>
#include <cmath>
#include "config.h"
#include "main.h"

// -- Global State Variables for this module --
unsigned long lastHeaterOnTime = 0, lastHeaterOffTime = 0;
unsigned long lastCoolerOnTime = 0, lastCoolerOffTime = 0;
unsigned long lastFanCycleTime = 0;
bool isFanCirculating = false;
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
const unsigned long FAN_CIRCULATE_ON_TIME_MS = FAN_CIRCULATE_ON_TIME_MINS * 60 * 1000;
const unsigned long FAN_CIRCULATE_OFF_TIME_MS = FAN_CIRCULATE_OFF_TIME_MINS * 60 * 1000;
const unsigned long MAX_RUN_LOCKOUT_MS = MAX_RUN_LOCKOUT_HOURS * 60 * 60 * 1000;

void initialize_logic_timers() {
  unsigned long now = currentTime();
  lastHeaterOffTime = now - (MIN_HEATER_OFF_TIME_MS + 1000);
  lastCoolerOffTime = now - (MIN_COOLER_OFF_TIME_MS + 1000);
  lastFanCycleTime = now;
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
    if (durationMs < (MIN_HEATER_RUN_TIME_MS - 1000)) return;

    float currentTempF = celsiusToFahrenheit(readTemperature());
    float tempChangeF = abs(currentTempF - cycleStartTempF);
    float durationHours = (float)durationMs / (1000.0 * 60.0 * 60.0);
    float rateF_PerHour = tempChangeF / durationHours;

    int bin = getPerformanceBin(cycleStartOutdoorTempF);
    int season = getCurrentSeason();

    if (isHeating) {
        int oldCount = performance.heatSamples[season][bin];
        performance.heatRate[season][bin] = ((performance.heatRate[season][bin] * oldCount) + rateF_PerHour) / (oldCount + 1);
        performance.heatSamples[season][bin]++;
    } else {
        int oldCount = performance.coolSamples[season][bin];
        performance.coolRate[season][bin] = ((performance.coolRate[season][bin] * oldCount) + rateF_PerHour) / (oldCount + 1);
        performance.coolSamples[season][bin]++;
    }
    saveSettings();
    cycleStartTime = 0;
}

void controlTemperature(float tempC, float targetTempC, float outdoorTempC) {
  if (systemLockedOut) {
    if(readRelay(HEATER_RELAY_PIN)) writeRelay(HEATER_RELAY_PIN, LOW);
    if(readRelay(COOLER_RELAY_PIN)) writeRelay(COOLER_RELAY_PIN, LOW);
    if(readRelay(FRESH_AIR_RELAY_PIN)) writeRelay(FRESH_AIR_RELAY_PIN, LOW);
    return;
  }

  unsigned long now = currentTime();
  bool isHeatingOn = readRelay(HEATER_RELAY_PIN);
  bool isCoolingOn = readRelay(COOLER_RELAY_PIN);
  bool isFreshAirOn = readRelay(FRESH_AIR_RELAY_PIN);
  
  float tempDeadbandC = (tempUnit == FAHRENHEIT) ? fahrenheitToCelsius(32.0 + TEMPERATURE_DEADBAND_F) - fahrenheitToCelsius(32.0) : TEMPERATURE_DEADBAND_F;
  float freshAirDiffC = (tempUnit == FAHRENHEIT) ? fahrenheitToCelsius(32.0 + FRESH_AIR_TEMP_DIFFERENTIAL_F) - fahrenheitToCelsius(32.0) : FRESH_AIR_TEMP_DIFFERENTIAL_F;

  if (systemMode == SYS_HEAT || (systemMode == SYS_AUTO && !isCoolingOn && !isFreshAirOn)) {
    if (isHeatingOn && (now - lastHeaterOnTime > MAX_HEATER_RUN_TIME_MS)) {
        writeRelay(HEATER_RELAY_PIN, LOW); lastHeaterOffTime = now; updatePerformanceData(true);
        if (heaterMaxRunTriggers > 0 && (now - firstHeaterMaxRunTriggerTime > MAX_RUN_LOCKOUT_MS)) { heaterMaxRunTriggers = 1; firstHeaterMaxRunTriggerTime = now; } 
        else { if (heaterMaxRunTriggers == 0) firstHeaterMaxRunTriggerTime = now; heaterMaxRunTriggers++; if (heaterMaxRunTriggers >= MAX_RUN_TRIGGER_COUNT) { systemLockedOut = true; } }
    } else if (tempC >= targetTempC && (isHeatingOn || isFreshAirOn)) {
      if (isHeatingOn && (now - lastHeaterOnTime > MIN_HEATER_RUN_TIME_MS)) { writeRelay(HEATER_RELAY_PIN, LOW); lastHeaterOffTime = now; updatePerformanceData(true); } 
      else if(isFreshAirOn) { writeRelay(FRESH_AIR_RELAY_PIN, LOW); }
    } else if (tempC < (targetTempC - tempDeadbandC) && !isHeatingOn) {
      if (outdoorTempC > (tempC + freshAirDiffC)) { writeRelay(FRESH_AIR_RELAY_PIN, HIGH); writeRelay(HEATER_RELAY_PIN, LOW); } 
      else { if (now - lastHeaterOffTime > MIN_HEATER_OFF_TIME_MS) { if (!readRelay(COOLER_RELAY_PIN)) { writeRelay(HEATER_RELAY_PIN, HIGH); lastHeaterOnTime = now; cycleStartTime = now; cycleStartTempF = celsiusToFahrenheit(tempC); cycleStartOutdoorTempF = celsiusToFahrenheit(outdoorTempC); } } }
    } 
  }

  if (systemMode == SYS_COOL || (systemMode == SYS_AUTO && !isHeatingOn && !isFreshAirOn)) {
    if (isCoolingOn && (now - lastCoolerOnTime > MAX_COOLER_RUN_TIME_MS)) {
        writeRelay(COOLER_RELAY_PIN, LOW); lastCoolerOffTime = now; updatePerformanceData(false);
        if (coolerMaxRunTriggers > 0 && (now - firstCoolerMaxRunTriggerTime > MAX_RUN_LOCKOUT_MS)) { coolerMaxRunTriggers = 1; firstCoolerMaxRunTriggerTime = now; } 
        else { if (coolerMaxRunTriggers == 0) firstCoolerMaxRunTriggerTime = now; coolerMaxRunTriggers++; if (coolerMaxRunTriggers >= MAX_RUN_TRIGGER_COUNT) { systemLockedOut = true; } }
    } else if (tempC <= targetTempC && (isCoolingOn || isFreshAirOn)) {
      if (isCoolingOn && (now - lastCoolerOnTime > MIN_COOLER_RUN_TIME_MS)) { writeRelay(COOLER_RELAY_PIN, LOW); lastCoolerOffTime = now; updatePerformanceData(false); } 
      else if (isFreshAirOn) { writeRelay(FRESH_AIR_RELAY_PIN, LOW); }
    } else if (tempC > (targetTempC + tempDeadbandC) && !isCoolingOn) {
      if (outdoorTempC < (tempC - freshAirDiffC)) { writeRelay(FRESH_AIR_RELAY_PIN, HIGH); writeRelay(COOLER_RELAY_PIN, LOW); } 
      else { if (now - lastCoolerOffTime > MIN_COOLER_OFF_TIME_MS) { if (!readRelay(HEATER_RELAY_PIN)) { writeRelay(COOLER_RELAY_PIN, HIGH); lastCoolerOnTime = now; cycleStartTime = now; cycleStartTempF = celsiusToFahrenheit(tempC); cycleStartOutdoorTempF = celsiusToFahrenheit(outdoorTempC); } } }
    }
  }
}

void controlFan(FanMode fanMode) {
  unsigned long now = currentTime();
  bool isHeatCoolOrFreshAirActive = (readRelay(HEATER_RELAY_PIN) || readRelay(COOLER_RELAY_PIN) || readRelay(FRESH_AIR_RELAY_PIN));

  if (isHeatCoolOrFreshAirActive) { if (!readRelay(FAN_RELAY_PIN)) { writeRelay(FAN_RELAY_PIN, HIGH); } return; }
  
  switch(fanMode) {
    case FAN_ON: if (!readRelay(FAN_RELAY_PIN)) { writeRelay(FAN_RELAY_PIN, HIGH); } break;
    case FAN_CIRCULATE:
      if (isFanCirculating) { if (now - lastFanCycleTime >= FAN_CIRCULATE_ON_TIME_MS) { writeRelay(FAN_RELAY_PIN, LOW); isFanCirculating = false; lastFanCycleTime = now; } } 
      else { if (now - lastFanCycleTime >= FAN_CIRCULATE_OFF_TIME_MS) { writeRelay(FAN_RELAY_PIN, HIGH); isFanCirculating = true; lastFanCycleTime = now; } }
      break;
    case FAN_AUTO: default: if (readRelay(FAN_RELAY_PIN)) { writeRelay(FAN_RELAY_PIN, LOW); } break;
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
  float maxHumidityLimit = calculateMaxHumidityForWindow(indoorTempC, outdoorTempC);
  float effectiveTarget = std::min(HUMIDITY_TARGET, maxHumidityLimit);
  bool isHumidityControlOn = readRelay(HUMIDITY_RELAY_PIN);

  if (humidity < (effectiveTarget - HUMIDITY_DEADBAND) && !isHumidityControlOn) { writeRelay(HUMIDITY_RELAY_PIN, HIGH); } 
  else if (humidity >= effectiveTarget && isHumidityControlOn) { writeRelay(HUMIDITY_RELAY_PIN, LOW); }
}