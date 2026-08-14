#include "health.h"
#include <Arduino.h>
#include <cmath>
#include <cstring>
#include "nvs_flash.h"
#include "nvs.h"
#include "config.h"
#include "events.h"
#include "hvac_logic.h"
#include "hvac_tests.h"
#include "main.h"
#include "relays.h"
#include "sensors.h"

// Lifetime counters, persisted as one small NVS blob ("health") under the
// batched flush policy. Layout changes are guarded by the size check on load.
struct HealthCounters {
  uint32_t filterFanMs;        // blower runtime since last filter reset
  uint16_t noResponseHeat;
  uint16_t noResponseCool;
  uint16_t capacityWarnHeat;
  uint16_t capacityWarnCool;
  uint16_t shortCycleWarn;
  uint16_t maxRunHeat;
  uint16_t maxRunCool;
  uint16_t abnormalResets;
  uint16_t sensorDropouts;
};
static HealthCounters hc = {};
static bool healthDirty = false;

// Runtime state
static unsigned long lastPollRealMs = 0;
static unsigned long observedCycleStart = 0;   // which cycle no-response already fired for
static bool noResponseFiredThisCycle = false;
static unsigned long cycleStarts[8] = {0};     // rolling short-cycle window (virtual ms)
static int cycleStartIdx = 0;
static bool prevRemoteFresh = false;
static bool prevRemoteEver = false;

// -- Pure rule evaluators (also exercised by boot self-tests) --
bool health_evalNoResponse(bool isHeating, unsigned long elapsedMs, float startTempF, float nowTempF) {
  if (elapsedMs < HEALTH_NO_RESPONSE_GRACE_MINS * 60000UL) return false;
  if (isnan(startTempF) || isnan(nowTempF)) return false;
  float delta = nowTempF - startTempF; // + = warmed, - = cooled
  if (isHeating)  return delta < -HEALTH_NO_RESPONSE_DELTA_F; // heating, yet got colder
  return delta > HEALTH_NO_RESPONSE_DELTA_F;                  // cooling, yet got warmer
}

bool health_evalCapacity(float rateFPerHr, float baselineAvg, int baselineSamples) {
  if (baselineSamples < HEALTH_CAPACITY_MIN_SAMPLES) return false;
  if (baselineAvg <= 0.1f) return false;
  return rateFPerHr < baselineAvg * HEALTH_CAPACITY_WARN_FRACTION;
}

// -- Persistence --
static void loadCounters() {
  nvs_handle_t h;
  if (nvs_open("hvac_cfg", NVS_READONLY, &h) != ESP_OK) return;
  size_t sz = sizeof(hc);
  if (nvs_get_blob(h, "health", &hc, &sz) != ESP_OK || sz != sizeof(hc)) {
    memset(&hc, 0, sizeof(hc)); // absent or layout change: start fresh
  }
  nvs_close(h);
}

static void saveCounters() {
  if (g_isTesting) return;
  nvs_handle_t h;
  if (nvs_open("hvac_cfg", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_blob(h, "health", &hc, sizeof(hc));
  nvs_commit(h);
  nvs_close(h);
  healthDirty = false;
}

void health_flushIfDirty() { if (healthDirty) saveCounters(); }

void health_begin() {
  loadCounters();
  lastPollRealMs = millis();
  esp_reset_reason_t r = esp_reset_reason();
  if (r != ESP_RST_POWERON && r != ESP_RST_SW) {
    hc.abnormalResets++;
    logEvent("HEALTH: abnormal reset (reason %d, lifetime %u)", (int)r, hc.abnormalResets);
    saveCounters(); // once per boot: negligible wear, and reboot loops must be visible
  }
}

// -- Event hooks from the control engine --
void health_reportCycleStart(bool isHeating) {
  unsigned long now = currentTime();
  cycleStarts[cycleStartIdx] = now;
  cycleStartIdx = (cycleStartIdx + 1) % 8;
  int recent = 0;
  for (int i = 0; i < 8; i++)
    if (cycleStarts[i] && now - cycleStarts[i] < 3600000UL) recent++;
  if (recent == HEALTH_SHORT_CYCLES_PER_HOUR) { // fire once as the threshold is crossed
    hc.shortCycleWarn++;
    healthDirty = true;
    logEvent("HEALTH: short-cycling (%d %s starts/hour)", recent, isHeating ? "heat" : "cool");
  }
  noResponseFiredThisCycle = false;
}

void health_reportCycleRate(bool isHeating, float rateFPerHr, float baselineAvg, int baselineSamples) {
  if (!health_evalCapacity(rateFPerHr, baselineAvg, baselineSamples)) return;
  if (isHeating) hc.capacityWarnHeat++; else hc.capacityWarnCool++;
  healthDirty = true;
  logEvent("HEALTH: %s capacity degraded (%.1fF/hr vs baseline %.1f)",
           isHeating ? "heating" : "cooling", rateFPerHr, baselineAvg);
}

void health_reportMaxRun(bool isHeating) {
  if (isHeating) hc.maxRunHeat++; else hc.maxRunCool++;
  healthDirty = true;
}

void health_poll() {
  // Filter runtime: real time whenever the blower is on.
  unsigned long nowReal = millis();
  if (readRelay(FAN_RELAY)) {
    unsigned long d = nowReal - lastPollRealMs;
    if (d < 60000UL) { hc.filterFanMs += d; healthDirty = true; }
  }
  lastPollRealMs = nowReal;

  // No-response: an active call that is losing ground temperature-wise.
  bool isHeating; unsigned long startMs; float startTempF;
  if (getActiveCycleInfo(isHeating, startMs, startTempF)) {
    if (startMs != observedCycleStart) { observedCycleStart = startMs; noResponseFiredThisCycle = false; }
    if (!noResponseFiredThisCycle &&
        health_evalNoResponse(isHeating, currentTime() - startMs, startTempF, lastIndoorF)) {
      noResponseFiredThisCycle = true;
      if (isHeating) hc.noResponseHeat++; else hc.noResponseCool++;
      healthDirty = true;
      logEvent("HEALTH: %s call NOT responding (%.1fF -> %.1fF)",
               isHeating ? "heat" : "cool", startTempF, lastIndoorF);
    }
  }

  // Indoor remote probe dropouts (fresh -> stale transition).
  bool fresh = sensors_remoteFresh();
  if (prevRemoteEver && prevRemoteFresh && !fresh) {
    hc.sensorDropouts++;
    healthDirty = true;
    logEvent("HEALTH: indoor remote probe went stale (dropout %u)", hc.sensorDropouts);
  }
  prevRemoteFresh = fresh;
  prevRemoteEver = prevRemoteEver || fresh;
}

void health_resetFilter() {
  hc.filterFanMs = 0;
  healthDirty = true;
  saveCounters();
  logEvent("filter runtime counter reset");
}

int health_warningCount() {
  int warns = 0;
  if (hc.noResponseHeat || hc.noResponseCool) warns++;
  if (hc.capacityWarnHeat || hc.capacityWarnCool) warns++;
  if (hc.shortCycleWarn) warns++;
  if (hc.maxRunHeat || hc.maxRunCool) warns++;
  if (hc.filterFanMs / 3600000UL >= FILTER_SERVICE_FAN_HOURS) warns++;
  if (relays_modio2FailureCount() > 0) warns++;
  if (hc.abnormalResets > 0) warns++;
  if (hc.sensorDropouts > 2) warns++;
  return warns;
}

static void row(const char* name, unsigned long value, unsigned long threshold, bool warn) {
  Serial.printf("  %-22s %8lu  thr %-6lu  %s\n", name, value, threshold, warn ? "[WARN]" : "[OK]");
}

void printHealthReport() {
  unsigned long filterHours = hc.filterFanMs / 3600000UL;
  Serial.println("=== Equipment Health (S.M.A.R.T.) ===");
  Serial.println("  attribute                 value  threshold  status");
  row("heat-no-response",   hc.noResponseHeat, 1, hc.noResponseHeat >= 1);
  row("cool-no-response",   hc.noResponseCool, 1, hc.noResponseCool >= 1);
  row("heat-capacity-warns", hc.capacityWarnHeat, 1, hc.capacityWarnHeat >= 1);
  row("cool-capacity-warns", hc.capacityWarnCool, 1, hc.capacityWarnCool >= 1);
  row("short-cycle-warns",  hc.shortCycleWarn, 1, hc.shortCycleWarn >= 1);
  row("max-run-trips-heat", hc.maxRunHeat, 1, hc.maxRunHeat >= 1);
  row("max-run-trips-cool", hc.maxRunCool, 1, hc.maxRunCool >= 1);
  row("filter-fan-hours",   filterHours, FILTER_SERVICE_FAN_HOURS, filterHours >= FILTER_SERVICE_FAN_HOURS);
  row("modio2-i2c-failures", relays_modio2FailureCount(), 1, relays_modio2FailureCount() > 0);
  row("abnormal-resets",    hc.abnormalResets, 1, hc.abnormalResets >= 1);
  row("sensor-dropouts",    hc.sensorDropouts, 3, hc.sensorDropouts > 2);
  Serial.printf("Overall: %d warning group(s). Rules are advisory - control is never interrupted.\n",
                health_warningCount());
  Serial.println("('health reset filter' after a filter change)");
}
