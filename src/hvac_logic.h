#ifndef HVAC_LOGIC_H
#define HVAC_LOGIC_H

#include "config.h"

// Function Declarations
void initialize_logic_timers();
// Dual setpoints: heat branch drives toward heatTargetC, cool branch toward
// coolTargetC. `mode` is the resolved effective mode (protections may differ
// from the user's systemMode).
void controlTemperature(float tempC, float heatTargetC, float coolTargetC, float outdoorTempC, SystemMode mode);
void controlFan(FanMode fanMode);
void controlHumidity(float humidity, float indoorTempC, float outdoorTempC);
void updatePerformanceData(bool isHeating);
int getPerformanceBin(float tempF);
void resetLockout();           // console `reset lockout`
bool coolerMinOffElapsed();    // service-mode compressor guard
// Health monitoring: expose the in-flight cycle (relay on + start snapshot).
bool getActiveCycleInfo(bool& isHeating, unsigned long& startTime, float& startTempF);

#endif // HVAC_LOGIC_H