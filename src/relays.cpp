#include "relays.h"
#include <Arduino.h>
#include <Wire.h>
#include "config.h"
#include "events.h"
#include "hvac_tests.h"

static const char* RELAY_LOG_NAMES[RELAY_COUNT] =
  {"heater", "cooler", "fan", "fresh-air", "humidifier", "spare"};

// Physical backing for each logical relay index in config.h.
struct RelayChannel {
  bool    onModIo2; // false = EVB onboard relay (GPIO), true = MOD-IO2 relay
  uint8_t index;    // GPIO number, or MOD-IO2 relay bit (0 or 1)
};

static const RelayChannel RELAY_MAP[RELAY_COUNT] = {
  { false, 10 }, // HEATER_RELAY     - EVB REL1
  { false, 11 }, // COOLER_RELAY     - EVB REL2
  { false, 22 }, // FAN_RELAY        - EVB REL3
  { false, 23 }, // FRESH_AIR_RELAY  - EVB REL4
  { true,  0  }, // HUMIDITY_RELAY   - MOD-IO2 relay 1
  { true,  1  }, // SPARE_RELAY      - MOD-IO2 relay 2
};

// MOD-IO2 has no reliable readback on Rev.B (command 0x43 exists only from
// firmware 3.02), so the driver is write-through with a cached state mask.
static uint8_t modio2StateMask = 0;
static bool modio2Present = false;
static int modio2Failures = 0;

int relays_modio2FailureCount() { return modio2Failures; }

// Firmware 3.xx/4.3 command: 0x40 = set both relays from data bits [1:0].
static bool modio2WriteMask(uint8_t mask) {
  Wire.beginTransmission(MODIO2_I2C_ADDRESS);
  Wire.write(0x40);
  Wire.write(mask & 0x03);
  return Wire.endTransmission() == 0;
}

void initRelays() {
  for (int i = 0; i < RELAY_COUNT; i++) {
    if (!RELAY_MAP[i].onModIo2) {
      pinMode(RELAY_MAP[i].index, OUTPUT);
      digitalWrite(RELAY_MAP[i].index, LOW);
    }
  }

  Wire.begin(UEXT_I2C_SDA_PIN, UEXT_I2C_SCL_PIN);
  modio2StateMask = 0;
  modio2Present = modio2WriteMask(0); // force known-off state; doubles as probe
  if (!modio2Present) {
    Serial.println("WARNING: MOD-IO2 not responding on I2C 0x21 - relays 5/6 (humidifier/spare) unavailable.");
  }
}

void writeRelay(int relay, bool value) {
  if (g_isTesting) { g_mockRelayStates[relay] = value; return; }
  const RelayChannel& ch = RELAY_MAP[relay];
  bool prev = readRelay(relay);
  if (!ch.onModIo2) {
    digitalWrite(ch.index, value);
    if (prev != value) logEvent("relay %s -> %s", RELAY_LOG_NAMES[relay], value ? "ON" : "off");
    return;
  }
  uint8_t newMask = value ? (modio2StateMask | (1 << ch.index))
                          : (modio2StateMask & ~(1 << ch.index));
  if (modio2WriteMask(newMask)) {
    modio2StateMask = newMask;
    modio2Present = true;
    if (prev != value) logEvent("relay %s -> %s", RELAY_LOG_NAMES[relay], value ? "ON" : "off");
  } else if (newMask != modio2StateMask) {
    modio2Failures++;
    Serial.printf("WARNING: MOD-IO2 write failed; relay %d not switched.\n", relay);
    logEvent("MOD-IO2 write FAILED (relay %s)", RELAY_LOG_NAMES[relay]);
  }
}

bool readRelay(int relay) {
  if (g_isTesting) return g_mockRelayStates[relay];
  const RelayChannel& ch = RELAY_MAP[relay];
  if (!ch.onModIo2) return digitalRead(ch.index);
  return (modio2StateMask >> ch.index) & 1;
}
