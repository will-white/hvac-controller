#ifndef HVAC_LOGIC_H
#define HVAC_LOGIC_H

#include "config.h"

// Function Declarations
void initialize_logic_timers();
void controlTemperature(float tempC, float targetTempC, float outdoorTempC);
void controlFan(FanMode fanMode);
void controlHumidity(float humidity, float indoorTempC, float outdoorTempC);
void updatePerformanceData(bool isHeating);
int getPerformanceBin(float tempF);

#endif // HVAC_LOGIC_H