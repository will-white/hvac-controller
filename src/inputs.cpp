#include "inputs.h"
#include <Arduino.h>
#include "config.h"
#include "events.h"

// All timing here is REAL millis: AC presence is a physical-world signal and
// must never be warped by `sim speed`.
static unsigned long lastAssertedMs[OPTO_COUNT] = {0};
static unsigned long lastPollMs = 0;
static bool seenEver[OPTO_COUNT] = {false};
static bool activeNow[OPTO_COUNT] = {false};

const char* optoRoleName(OptoRole r) {
  switch (r) {
    case OPTO_HEAT_PROOF: return "heat-proof";
    case OPTO_COOL_PROOF: return "cool-proof";
    case OPTO_FAN_PROOF:  return "fan-proof";
    case OPTO_WINDOW:     return "window";
    default:              return "none";
  }
}

void inputs_begin() {
  // Internal pull-up is harmless in parallel with the board's own; the opto
  // transistor sinks the pin low when its input is energized.
  for (int i = 0; i < OPTO_COUNT; i++) pinMode(OPTO_PINS[i], INPUT_PULLUP);
  lastPollMs = millis();
}

void inputs_poll() {
  unsigned long now = millis();
  // Blocking loop segments (the weather HTTP fetch can stall for seconds) leave
  // an unsampled gap; that gap must not read as "signal lost", or a not-proven
  // warning could false-trip mid-call. After a stall, anything active before it
  // keeps a fresh half-window to be re-observed in.
  bool stalled = (now - lastPollMs) > OPTO_AC_HOLD_MS / 2;
  lastPollMs = now;
  for (int i = 0; i < OPTO_COUNT; i++) {
    if (OPTO_ROLES[i] == OPTO_NONE) continue;
    bool asserted = (digitalRead(OPTO_PINS[i]) == LOW) == OPTO_ACTIVE_LOW;
    if (!asserted && stalled && activeNow[i]) {
      lastAssertedMs[i] = now - OPTO_AC_HOLD_MS / 2;
    }
    if (asserted) {
      lastAssertedMs[i] = now;
      if (!seenEver[i]) {
        seenEver[i] = true;
        logEvent("opto OPT%d (%s): first signal - rules armed", i + 1, optoRoleName(OPTO_ROLES[i]));
      }
    }
    bool act = seenEver[i] && (now - lastAssertedMs[i] <= OPTO_AC_HOLD_MS);
    if (act != activeNow[i]) {
      activeNow[i] = act;
      // Only the window contact logs transitions: the proof inputs toggle with
      // every cycle and the relay transitions already tell that story.
      if (OPTO_ROLES[i] == OPTO_WINDOW) logEvent("window contact: signal %s", act ? "present" : "absent");
    }
  }
}

bool inputs_isActive(int idx)       { return idx >= 0 && idx < OPTO_COUNT && activeNow[idx]; }
bool inputs_rawLevel(int idx)       { return digitalRead(OPTO_PINS[idx]); }
bool inputs_isCommissioned(int idx) { return idx >= 0 && idx < OPTO_COUNT && seenEver[idx]; }

int inputs_forRole(OptoRole role) {
  for (int i = 0; i < OPTO_COUNT; i++)
    if (OPTO_ROLES[i] == role) return i;
  return -1;
}

uint8_t inputs_commissionedMask() {
  uint8_t m = 0;
  for (int i = 0; i < OPTO_COUNT; i++)
    if (seenEver[i]) m |= (1 << i);
  return m;
}

void inputs_setCommissioned(uint8_t mask) {
  for (int i = 0; i < OPTO_COUNT; i++)
    if (mask & (1 << i)) seenEver[i] = true; // restore silently (no re-arm event)
}
