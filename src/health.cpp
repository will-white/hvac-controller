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
#include "inputs.h"
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
  uint16_t callNotProven[3]; // heat, cool, fan: call active, no 24VAC feedback
  uint16_t phantomRun[3];    // feedback active with NO call (welded relay)
  uint8_t  optoCommissioned; // which inputs have ever seen a signal
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

bool health_evalCallProven(bool commissioned, unsigned long callElapsedMs, bool proofActive) {
  if (!commissioned) return false; // input never wired: stay quiet
  if (callElapsedMs < OPTO_PROVE_GRACE_SECS * 1000UL) return false;
  return !proofActive;
}

bool health_evalPhantomRun(bool commissioned, bool callOn, unsigned long proofElapsedMs, bool proofActive) {
  if (!commissioned || callOn || !proofActive) return false;
  return proofElapsedMs >= OPTO_PHANTOM_GRACE_SECS * 1000UL;
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
  inputs_setCommissioned(hc.optoCommissioned); // re-arm proving across reboots
  lastPollRealMs = millis();
  esp_reset_reason_t r = esp_reset_reason();
  // USB/JTAG resets are host-initiated (esptool after every flash) — deliberate,
  // not a reboot loop. Abnormal = panic, any watchdog, brownout, etc.
  if (r != ESP_RST_POWERON && r != ESP_RST_SW && r != ESP_RST_USB && r != ESP_RST_JTAG) {
    hc.abnormalResets++;
    logEvent("HEALTH: abnormal reset (%s, lifetime %u)", resetReasonName((int)r), hc.abnormalResets);
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

  // Opto call-proving (real time: 24VAC presence is a physical signal).
  // Advisory like every rule here — control is never interrupted.
  static const int      proofRelays[3] = { HEATER_RELAY, COOLER_RELAY, FAN_RELAY };
  static const OptoRole proofRoles[3]  = { OPTO_HEAT_PROOF, OPTO_COOL_PROOF, OPTO_FAN_PROOF };
  static const char*    proofNames[3]  = { "heat", "cool", "fan" };
  static unsigned long  callOnSince[3] = {0};    // 0 = call off
  static unsigned long  proofOnSince[3] = {0};   // feedback active while call off
  static bool warnedThisCall[3] = {false};
  static bool warnedThisPhantom[3] = {false};
  for (int k = 0; k < 3; k++) {
    int idx = inputs_forRole(proofRoles[k]);
    if (idx < 0) continue;
    bool callOn = readRelay(proofRelays[k]);
    bool proof = inputs_isActive(idx);
    bool comm = inputs_isCommissioned(idx);
    if (callOn) {
      if (!callOnSince[k]) { callOnSince[k] = nowReal ? nowReal : 1; warnedThisCall[k] = false; }
      if (!warnedThisCall[k] && health_evalCallProven(comm, nowReal - callOnSince[k], proof)) {
        warnedThisCall[k] = true;
        hc.callNotProven[k]++;
        healthDirty = true;
        logEvent("HEALTH: %s call NOT proven (no 24VAC feedback)", proofNames[k]);
      }
      proofOnSince[k] = 0;
    } else {
      callOnSince[k] = 0;
      if (proof) {
        if (!proofOnSince[k]) { proofOnSince[k] = nowReal ? nowReal : 1; warnedThisPhantom[k] = false; }
        if (!warnedThisPhantom[k] && health_evalPhantomRun(comm, false, nowReal - proofOnSince[k], proof)) {
          warnedThisPhantom[k] = true;
          hc.phantomRun[k]++;
          healthDirty = true;
          logEvent("HEALTH: %s feedback active with NO call (welded relay?)", proofNames[k]);
        }
      } else {
        proofOnSince[k] = 0;
      }
    }
  }
  uint8_t commMask = inputs_commissionedMask();
  if (commMask != hc.optoCommissioned) { hc.optoCommissioned = commMask; healthDirty = true; }

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

void health_resetCounters() {
  memset(&hc, 0, sizeof(hc));
  healthDirty = true;
  saveCounters();
  logEvent("all health counters reset");
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
  if (hc.callNotProven[0] || hc.callNotProven[1] || hc.callNotProven[2]) warns++;
  if (hc.phantomRun[0] || hc.phantomRun[1] || hc.phantomRun[2]) warns++;
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
  row("heat-call-not-proven", hc.callNotProven[0], 1, hc.callNotProven[0] >= 1);
  row("cool-call-not-proven", hc.callNotProven[1], 1, hc.callNotProven[1] >= 1);
  row("fan-call-not-proven",  hc.callNotProven[2], 1, hc.callNotProven[2] >= 1);
  unsigned long phantoms = hc.phantomRun[0] + hc.phantomRun[1] + hc.phantomRun[2];
  row("phantom-equip-runs", phantoms, 1, phantoms >= 1);
  Serial.printf("  opto proving armed: %s%s%s%s\n",
                inputs_isCommissioned(inputs_forRole(OPTO_HEAT_PROOF)) ? "heat " : "",
                inputs_isCommissioned(inputs_forRole(OPTO_COOL_PROOF)) ? "cool " : "",
                inputs_isCommissioned(inputs_forRole(OPTO_FAN_PROOF)) ? "fan " : "",
                hc.optoCommissioned ? "" : "(none - inputs not wired yet)");
  Serial.printf("Overall: %d warning group(s). Rules are advisory - control is never interrupted.\n",
                health_warningCount());
  Serial.println("('health reset filter' after a filter change)");
}
