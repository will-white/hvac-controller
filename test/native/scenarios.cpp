// Native-only scenario tests: drive the real controlTick() through simulated
// time with the mock layer (g_isTesting), checking behaviour that spans many
// ticks — which the on-device unit suite can't do at boot.
#include <Arduino.h>
#include "config.h"
#include "hvac_logic.h"
#include "hvac_tests.h"
#include "main.h"
#include "relays.h"
#include "sensors.h"
#include "setpoints.h"

// Mock helpers defined in hvac_tests.cpp.
void resetHvacState();
void setMockTime(int month, int day, int dayOfWeek, int hour, int min);
void advanceMockMillis(unsigned long ms);
void setMockSensors(float indoorF, float outdoorF);

static int failures = 0;

static void check(const char* name, bool cond) {
  printf("Scenario: %s ... %s\n", name, cond ? "[PASS]" : "[FAIL]");
  if (!cond) failures++;
}

// Advance simulated time by `mins` minutes of 5 s control ticks.
static void runMinutes(float mins) {
  int n = (int)(mins * 60000.0f / CONTROL_INTERVAL_MS + 0.5f);
  for (int i = 0; i < n; i++) {
    advanceMockMillis(CONTROL_INTERVAL_MS);
    controlTick();
  }
}

static void tick() { runMinutes(CONTROL_INTERVAL_MS / 60000.0f); }

static bool heatOn() { return readRelay(HEATER_RELAY); }
static bool coolOn() { return readRelay(COOLER_RELAY); }

static void begin(const char* title, SystemMode mode) {
  printf("  --- %s ---\n", title);
  g_isTesting = true;
  resetHvacState();
  systemInFaultState = false;
  systemLockedOut = false;
  indoorReadingMissing = false;
  vacationModeActive = false;
  clearHold();
  systemMode = mode;
  setMockTime(1, 5, 1, 7, 0); // Monday 07:00: weekday Wake, heat-to 70 / cool-to 75
}

static void neverCommissioned() {
  // Must run first: nothing has reported yet in this process.
  begin("Never commissioned: wait quietly", SYS_HEAT);
  setMockSensors(NAN, 30);
  runMinutes(10);
  check("1. no fault without any sensor ever reporting", !systemInFaultState);
  check("2. not flagged as a missing reading either", !indoorReadingMissing);
  check("3. heat stays off", !heatOn());
}

static void heatSurvivesProbeOutage() {
  begin("Hub outage during heating does not latch", SYS_HEAT);
  sensors_injectRemote(66, 40); // the probe has reported: commissioned
  setMockSensors(66, 30);
  tick();
  check("1. cold house calls for heat", heatOn());
  runMinutes(2);
  setMockSensors(NAN, 30); // probe stale, no local sensor
  tick();
  check("2. heat stops when the reading disappears", !heatOn());
  check("3. flagged as missing, not latched as a fault", indoorReadingMissing && !systemInFaultState);
  runMinutes(45);
  check("4. still waiting, still not latched, 45 min later", !heatOn() && !systemInFaultState);
  setMockSensors(66, 30);
  tick();
  check("5. reading back: flag clears", !indoorReadingMissing);
  check("6. heat resumes on its own (min-off long elapsed)", heatOn());
}

static void compressorMinOffAcrossOutage() {
  begin("Brief outage while cooling keeps compressor min-off", SYS_COOL);
  sensors_injectRemote(80, 50);
  setMockSensors(80, 85);
  runMinutes(MIN_COOLER_OFF_TIME_MINS + 1); // boot-time compressor lockout
  check("1. warm house is cooling", coolOn());
  runMinutes(MIN_COOLER_RUN_TIME_MINS + 1);
  setMockSensors(NAN, 85);
  tick();
  check("2. cooling stops when the reading disappears", !coolOn());
  runMinutes(1);
  setMockSensors(80, 85);
  runMinutes(MIN_COOLER_OFF_TIME_MINS - 2);
  check("3. reading back after 1 min: compressor still resting", !coolOn());
  runMinutes(2);
  check("4. compressor restarts once min-off has elapsed", coolOn());
}

static void implausibleReadingStillLatches() {
  begin("Implausible reading latches a fault", SYS_HEAT);
  sensors_injectRemote(66, 40);
  setMockSensors(66, 30);
  tick();
  check("1. heating", heatOn());
  setMockSensors(158, 30); // e.g. a Fahrenheit payload read as Celsius
  tick();
  check("2. fault latched, heat off", systemInFaultState && !heatOn());
  setMockSensors(66, 30);
  runMinutes(10);
  check("3. good readings do not clear it (needs 'reset fault')", systemInFaultState && !heatOn());
}

static void compressorMinOffAfterFaultReset() {
  begin("Fault reset honours compressor min-off", SYS_COOL);
  sensors_injectRemote(80, 50);
  setMockSensors(80, 85);
  runMinutes(MIN_COOLER_OFF_TIME_MINS + 1);
  runMinutes(MIN_COOLER_RUN_TIME_MINS + 1);
  check("1. cooling", coolOn());
  setMockSensors(150, 85);
  tick();
  check("2. fault cuts the compressor", systemInFaultState && !coolOn());
  runMinutes(1);
  systemInFaultState = false; // operator acknowledges at once
  setMockSensors(80, 85);
  runMinutes(MIN_COOLER_OFF_TIME_MINS - 2);
  check("3. compressor waits out min-off after the reset", !coolOn());
  runMinutes(2);
  check("4. then cools again", coolOn());
}

int runScenarios() {
  failures = 0;
  printf("\n--- Native scenario tests ---\n");
  neverCommissioned();
  heatSurvivesProbeOutage();
  compressorMinOffAcrossOutage();
  implausibleReadingStillLatches();
  compressorMinOffAfterFaultReset();
  g_isTesting = false;
  printf("--- Native scenarios complete: %d failure(s) ---\n", failures);
  return failures;
}
