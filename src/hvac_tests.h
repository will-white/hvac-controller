#ifndef HVAC_TESTS_H
#define HVAC_TESTS_H

#include "config.h" // For TimeInfo

// Extern declarations for global testing variables
extern bool g_isTesting;
extern unsigned long g_mockMillis;
extern TimeInfo g_mockTime;
extern float g_mockIndoorTempF;
extern float g_mockOutdoorTempF;
extern bool g_mockRelayStates[10];

void runTests();

#endif // HVAC_TESTS_H