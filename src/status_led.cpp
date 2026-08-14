#include "status_led.h"
#include <Arduino.h>
#include "config.h"
#include "console.h"
#include "health.h"
#include "main.h"

// Each pattern is 16 slots of 125 ms (a 2 s cycle), LSB first. Runs on REAL
// millis: the codes are for a human standing at the panel, and must keep
// blinking truthfully under `sim speed` and even while control is suspended.
static const uint16_t PAT_FAULT     = 0x5555; // 4 Hz continuous
static const uint16_t PAT_LOCKOUT   = 0x0005; // two quick flashes, long gap
static const uint16_t PAT_SERVICE   = 0xFFFF; // solid on
static const uint16_t PAT_HEALTH    = 0x0015; // three quick flashes, long gap
static const uint16_t PAT_HEARTBEAT = 0x0001; // single 125 ms blip

void statusLed_begin() {
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, STATUS_LED_ACTIVE_HIGH ? LOW : HIGH);
}

void statusLed_poll() {
  uint16_t pat;
  if (systemInFaultState)                pat = PAT_FAULT;
  else if (systemLockedOut)              pat = PAT_LOCKOUT;
  else if (console_testModeActive())     pat = PAT_SERVICE;
  else if (health_warningCount() > 0)    pat = PAT_HEALTH;
  else                                   pat = PAT_HEARTBEAT;

  int slot = (millis() / 125UL) % 16;
  bool on = (pat >> slot) & 1;
  digitalWrite(STATUS_LED_PIN, on == STATUS_LED_ACTIVE_HIGH ? HIGH : LOW);
}
