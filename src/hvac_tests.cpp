#include "hvac_tests.h"
#include <Arduino.h>
#include <cmath>
#include <cstring>
#include "config.h"
#include "main.h"
#include "hvac_logic.h"
#include "health.h"
#include "ota.h"
#include "setpoints.h"
#include "utils.h"

// -- Mocking infrastructure for tests --
bool g_isTesting = false;
unsigned long g_mockMillis = 0;
TimeInfo g_mockTime;
float g_mockIndoorTempF, g_mockOutdoorTempF;
bool g_mockRelayStates[10] = {LOW,LOW,LOW,LOW,LOW,LOW,LOW,LOW,LOW,LOW};

void setMockTime(int month, int day, int dayOfWeek, int hour, int min) {
    g_mockTime.month = month; g_mockTime.day = day;
    g_mockTime.dayOfWeek = dayOfWeek; g_mockTime.hour = hour; g_mockTime.minute = min;
}
void advanceMockMillis(unsigned long ms) { g_mockMillis += ms; }
void setMockSensors(float iF, float oF) { g_mockIndoorTempF = iF; g_mockOutdoorTempF = oF; }
void resetHvacState() {
  for (int i=0; i<10; i++) g_mockRelayStates[i] = LOW;
  // Nonzero: a cycle starting at mock time 0 would collide with the
  // cycleStartTime==0 "no active cycle" sentinel and never reach the learner.
  g_mockMillis = 1000000;
  systemLockedOut = false;
  initialize_logic_timers();
}

static int g_testFailures = 0;

void test(const char* testName, bool condition) {
  if (!condition) g_testFailures++;
  Serial.print("Test: "); Serial.print(testName); Serial.print(" ... ");
  Serial.println(condition ? "[PASS]" : "[FAIL]");
}

void testSchedulingLogic() {
  Serial.println("  --- Testing Setpoint Resolution ---");
  setMockTime(1, 1, 1, 7, 0); // Monday 7:00
  ResolvedTargets r = resolveTargets(70.0, NAN);
  test("    1. Weekday Wake targets", fabs(r.heatSetpointF - 70.0) < 0.01 && fabs(r.coolSetpointF - 75.0) < 0.01 && r.source == SRC_SCHEDULE);
  setMockTime(1, 1, 3, 10, 0); // Wednesday 10:00
  r = resolveTargets(70.0, NAN);
  test("    2. Weekday Away targets", fabs(r.heatSetpointF - 64.0) < 0.01 && fabs(r.coolSetpointF - 79.0) < 0.01);
  // Monday 5:00 AM is before the first weekday entry (6:30): the target must
  // carry over Sunday night's last entry (23:00, 69/74), not jump to Wake.
  setMockTime(1, 1, 1, 5, 0);
  r = resolveTargets(70.0, NAN);
  test("    3. Pre-dawn carries over previous night", fabs(r.heatSetpointF - 69.0) < 0.01 && r.source == SRC_CARRYOVER);
  vacationModeActive = true;
  r = resolveTargets(70.0, NAN);
  test("    4. Vacation heat/cool pair", fabs(r.heatSetpointF - 55.0) < 0.01 && fabs(r.coolSetpointF - 85.0) < 0.01 && r.source == SRC_VACATION);
  vacationModeActive = false;
}

void testProtectionClamps() {
  Serial.println("  --- Testing Protection Clamps ---");
  setMockTime(1, 1, 3, 10, 0);
  ResolvedTargets r = resolveTargets(40.0, NAN); // dangerously cold indoors
  test("    1. Freeze protection engages", r.source == SRC_FREEZE_PROTECT && r.effectiveMode == SYS_HEAT && fabs(r.heatSetpointF - FREEZE_PROTECT_TARGET_F) < 0.01);
  r = resolveTargets(60.0, NAN);
  test("    2. Freeze protection releases", r.source != SRC_FREEZE_PROTECT);
  r = resolveTargets(95.0, NAN); // dangerously hot indoors
  test("    3. Heat protection engages", r.source == SRC_HEAT_PROTECT && r.effectiveMode == SYS_COOL && fabs(r.coolSetpointF - HEAT_PROTECT_TARGET_F) < 0.01);
  r = resolveTargets(70.0, NAN);
  test("    4. Heat protection releases", r.source != SRC_HEAT_PROTECT);
}

void testSmartRecovery() {
  Serial.println("  --- Testing Smart Recovery ---");
  memset(&performance, 0, sizeof(performance));
  performance.heatRate[0][3] = 6.0;   // winter, 30-40F outdoor bin: 6F/hr
  performance.heatSamples[0][3] = 5;
  // Monday 5:30 AM: base is carryover (69F). Next entry is Wake 6:30 (70F),
  // 60 min out. Rise needed from 64F indoor = 6F -> lead = 60 min: engage.
  setMockTime(1, 5, 1, 5, 30);
  ResolvedTargets r = resolveTargets(64.0, 35.0);
  test("    1. Recovery engages at lead time", r.source == SRC_RECOVERY && fabs(r.heatSetpointF - 70.0) < 0.01);
  // 4:00 AM: 150 min out > 60 min lead: stay on carryover.
  setMockTime(1, 5, 1, 4, 0);
  r = resolveTargets(64.0, 35.0);
  test("    2. Too early - no recovery", r.source == SRC_CARRYOVER && fabs(r.heatSetpointF - 69.0) < 0.01);
  memset(&performance, 0, sizeof(performance));
}

void testHealthRules() {
  Serial.println("  --- Testing Health (S.M.A.R.T.) Rules ---");
  test("    1. No-response trips (heat call, temp falling)", health_evalNoResponse(true, 11*60000UL, 68.0, 67.2));
  test("    2. No-response quiet inside grace period", !health_evalNoResponse(true, 5*60000UL, 68.0, 67.2));
  test("    3. No-response quiet when responding", !health_evalNoResponse(true, 11*60000UL, 68.0, 68.6));
  test("    4. Cool no-response trips when warming", health_evalNoResponse(false, 11*60000UL, 76.0, 76.8));
  test("    5. Capacity warns below 60%% of baseline", health_evalCapacity(2.0, 5.0, 10));
  test("    6. Capacity quiet with sparse baseline", !health_evalCapacity(2.0, 5.0, 2));
}

void testOptoProofRules() {
  Serial.println("  --- Testing Opto Call-Proving Rules ---");
  test("    1. Unwired input never warns", !health_evalCallProven(false, 10*60000UL, false));
  test("    2. Not-proven trips past grace", health_evalCallProven(true, 2*60000UL, false));
  test("    3. Quiet inside grace period", !health_evalCallProven(true, 30000UL, false));
  test("    4. Quiet when feedback present", !health_evalCallProven(true, 2*60000UL, true));
  test("    5. Phantom run trips past grace", health_evalPhantomRun(true, false, 6*60000UL, true));
  test("    6. Phantom quiet while call is on", !health_evalPhantomRun(true, true, 6*60000UL, true));
  test("    7. Phantom quiet when unwired", !health_evalPhantomRun(false, false, 6*60000UL, true));
}

void testPerformanceLearning() {
    Serial.println("  --- Testing Seasonal Performance Learning ---");
    resetHvacState();
    memset(&performance, 0, sizeof(performance));
    float heatC = fahrenheitToCelsius(70);
    float coolC = fahrenheitToCelsius(76);
    const unsigned long MIN_HEATER_RUN_TIME_MS = MIN_HEATER_RUN_TIME_MINS * 60 * 1000;
    setMockTime(1, 15, 2, 10, 0); // Jan 15 (Winter, Season 0)
    setMockSensors(68, 35);
    controlTemperature(fahrenheitToCelsius(68), heatC, coolC, fahrenheitToCelsius(35), SYS_AUTO);
    advanceMockMillis(MIN_HEATER_RUN_TIME_MS + 5000);
    setMockSensors(71, 35);
    controlTemperature(fahrenheitToCelsius(71), heatC, coolC, fahrenheitToCelsius(35), SYS_AUTO);
    test("    1. Heat rate learned in Winter slot", performance.heatRate[0][3] > 0.1);
    test("    2. Heat rate NOT learned in Summer slot", fabs(performance.heatRate[2][3]) < 0.01);
}

// ... other test suites (min/max time, lockout, etc.) ...

void testOtaHelpers() {
  Serial.println("  --- Testing Update Helpers ---");
  char sha[48], ver[32];
  const char* good = "{\"sha\": \"20ba7d4c0ffee0000000000000000000000000ab\", \"version\": \"r12-20ba7d4\"}";
  test("    1. Manifest parses sha + version",
       ota_parseManifest(good, sha, sizeof(sha), ver, sizeof(ver)) &&
       !strcmp(sha, "20ba7d4c0ffee0000000000000000000000000ab") && !strcmp(ver, "r12-20ba7d4"));
  test("    2. Manifest rejects short sha", !ota_parseManifest("{\"sha\":\"20ba7d4\"}", sha, sizeof(sha), ver, sizeof(ver)));
  test("    3. Manifest rejects HTML (redirect page)", !ota_parseManifest("<html>302</html>", sha, sizeof(sha), ver, sizeof(ver)));
  // Minimal ESP32-C6 app image prefix: header magic, chip id 0x000D at 12,
  // app-descriptor magic 0xABCD5432 at 32 (little-endian).
  uint8_t img[40] = {0};
  img[0] = 0xE9; img[12] = 0x0D;
  img[32] = 0x32; img[33] = 0x54; img[34] = 0xCD; img[35] = 0xAB;
  test("    4. Accepts a C6 app image", ota_isAppImage(img, sizeof(img)));
  img[33] = 0x00;
  test("    5. Rejects bootloader/factory image (no app descriptor)", !ota_isAppImage(img, sizeof(img)));
  img[33] = 0x54; img[12] = 0x05;
  test("    6. Rejects another chip's image", !ota_isAppImage(img, sizeof(img)));
}

int runTests() {
  g_isTesting = true;
  g_testFailures = 0;

  // These tests run on every boot and mutate real state (testPerformanceLearning
  // wipes `performance` outright). Snapshot everything they touch and restore it
  // after, so learned data and settings survive the self-test.
  HvacPerformance savedPerformance = performance;
  Schedule savedSchedule = programSchedule;
  bool savedVacation = vacationModeActive;

  Serial.println("\n--- Starting Self-Test Suite ---");
  testSchedulingLogic();
  testProtectionClamps();
  testSmartRecovery();
  testHealthRules();
  testOptoProofRules();
  testPerformanceLearning();
  testOtaHelpers();
  // ... call other test suites ...

  performance = savedPerformance;
  programSchedule = savedSchedule;
  vacationModeActive = savedVacation;
  g_isTesting = false;
  Serial.printf("--- Self-Test Suite Complete: %d failure(s) ---\n\n", g_testFailures);
  return g_testFailures;
}
