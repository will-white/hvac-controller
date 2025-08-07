#include "hvac_tests.h"
#include <Arduino.h>
#include "config.h"
#include "main.h"
#include "hvac_logic.h"

extern Schedule programSchedule;
extern bool vacationModeActive;
extern float currentTargetTemperature;
extern FanMode currentFanMode;

void setMockTime(int month, int day, int dayOfWeek, int hour, int min) { 
    g_mockTime.month = month; g_mockTime.day = day; 
    g_mockTime.dayOfWeek = dayOfWeek; g_mockTime.hour = hour; g_mockTime.minute = min; 
}
void advanceMockMillis(unsigned long ms) { g_mockMillis += ms; }
void setMockSensors(float iF, float oF) { g_mockIndoorTempF = iF; g_mockOutdoorTempF = oF; }
void resetHvacState() {
  for (int i=0; i<10; i++) g_mockRelayStates[i] = LOW;
  g_mockMillis = 0;
  systemLockedOut = false;
  initialize_logic_timers();
}

void test(const char* testName, bool condition) {
  Serial.print("Test: "); Serial.print(testName); Serial.print(" ... ");
  Serial.println(condition ? "[PASS]" : "[FAIL]");
}

void testSchedulingLogic() {
  Serial.println("  --- Testing Scheduling Logic ---");
  setMockTime(1, 1, 1, 7, 0); getCurrentScheduleSettings();
  test("    1. Weekday Wake Temp", abs(currentTargetTemperature - 70.0) < 0.01);
  setMockTime(1, 1, 3, 10, 0); getCurrentScheduleSettings();
  test("    2. Weekday Away Temp", abs(currentTargetTemperature - 75.0) < 0.01);
  vacationModeActive = true; getCurrentScheduleSettings();
  test("    3. Vacation Mode Temp", abs(currentTargetTemperature - programSchedule.vacation.targetTemperature) < 0.01);
  vacationModeActive = false;
}

void testPerformanceLearning() {
    Serial.println("  --- Testing Seasonal Performance Learning ---");
    resetHvacState();
    memset(&performance, 0, sizeof(performance)); 
    float targetC = fahrenheitToCelsius(70);
    const unsigned long MIN_HEATER_RUN_TIME_MS = MIN_HEATER_RUN_TIME_MINS * 60 * 1000;
    setMockTime(1, 15, 2, 10, 0); // Jan 15 (Winter, Season 0)
    setMockSensors(68, 35);
    controlTemperature(fahrenheitToCelsius(68), targetC, fahrenheitToCelsius(35));
    advanceMockMillis(MIN_HEATER_RUN_TIME_MS + 5000); 
    setMockSensors(71, 35);
    controlTemperature(fahrenheitToCelsius(71), targetC, fahrenheitToCelsius(35));
    test("    1. Heat rate learned in Winter slot", performance.heatRate[0][3] > 0.1);
    test("    2. Heat rate NOT learned in Summer slot", abs(performance.heatRate[2][3]) < 0.01);
}

// ... other test suites (min/max time, lockout, etc.) ...

void runTests() {
  g_isTesting = true;
  Serial.println("\n--- Starting Self-Test Suite ---");
  testSchedulingLogic();
  testPerformanceLearning();
  // ... call other test suites ...
  g_isTesting = false;
  Serial.println("--- Self-Test Suite Complete ---\n");
}