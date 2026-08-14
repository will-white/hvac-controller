#include <Arduino.h>
#include <Wire.h>
#include "esp_task_wdt.h"
#include "config.h"

// Standalone hardware bring-up sketch for the relay stack:
//   - ESP32-C6-EVB onboard relays REL1-REL4 (GPIO driven)
//   - MOD-IO2 Rev.B relays 1-2 over UEXT I2C (address 0x21, command 0x40)
// Cycles each relay on for 1 s in turn. Watch the per-relay status LEDs.
// Reminder: MOD-IO2 relays only actuate with 12V DC on the MOD-IO2 barrel jack.
//
// SAFETY NOTE (2026-08-14 bring-up): the EVB relays fail safe — a reset drops
// the GPIOs and every contact opens. The MOD-IO2 does NOT: it holds its last
// commanded mask on its own 12V rail, so if this sketch hangs mid-cycle its
// relay stays CLOSED forever. Hence the task watchdog below — a hang reboots
// and setup() re-writes mask 0, bounding the latch to WDT_TIMEOUT_SECS instead
// of "until someone notices". Keep the WDT armed in any sketch that drives the
// expander.

const int evb_relay_gpios[] = {10, 11, 22, 23};
const char* evb_relay_names[] = {"REL1/Heater", "REL2/Cooler", "REL3/Fan", "REL4/FreshAir"};
const int num_evb_relays = sizeof(evb_relay_gpios) / sizeof(evb_relay_gpios[0]);

static uint32_t cycleCount = 0;

// Wire.endTransmission() status -> text, so a bus fault names itself.
static const char* i2cErrText(uint8_t err) {
  switch (err) {
    case 0: return "ok";
    case 1: return "data too long";
    case 2: return "NACK on address";
    case 3: return "NACK on data";
    case 4: return "other error";
    case 5: return "timeout";
    default: return "unknown";
  }
}

static uint8_t modIo2WriteRaw(uint8_t mask) {
  Wire.beginTransmission(MODIO2_I2C_ADDRESS);
  Wire.write(0x40);
  Wire.write(mask & 0x03);
  return Wire.endTransmission();
}

// Retries, because leaving the expander latched is the failure we care about.
static bool setModIo2Relays(uint8_t mask, const char* what) {
  for (int attempt = 1; attempt <= 3; attempt++) {
    uint8_t err = modIo2WriteRaw(mask);
    if (err == 0) return true;
    Serial.printf("  I2C %s mask=0x%02X attempt %d FAILED: %s\n",
                  what, mask, attempt, i2cErrText(err));
    esp_task_wdt_reset();
    delay(20);
  }
  return false;
}

// Feeds the watchdog while waiting; delay() alone does not.
static void holdFor(unsigned long ms) {
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    esp_task_wdt_reset();
    delay(10);
  }
}

static void allRelaysOff() {
  for (int i = 0; i < num_evb_relays; i++) digitalWrite(evb_relay_gpios[i], LOW);
  setModIo2Relays(0, "all-off");
}

void setup() {
  Serial.begin(115200);
  // Never let a missing/closed USB host stall the loop with a relay energized:
  // with no timeout, HWCDC writes block once the TX ring fills.
  Serial.setTxTimeoutMs(0);
  // USB-Serial-JTAG console: don't block forever waiting for a host.
  for (unsigned long t0 = millis(); !Serial && millis() - t0 < 3000; ) delay(10);

  for (int i = 0; i < num_evb_relays; i++) {
    pinMode(evb_relay_gpios[i], OUTPUT);
    digitalWrite(evb_relay_gpios[i], LOW);
  }

  // IDF 5 / Arduino core 3.x: the task WDT is already initialized at startup,
  // so reconfigure it rather than init'ing a second time.
  esp_task_wdt_config_t twdt_config = {};
  twdt_config.timeout_ms = WDT_TIMEOUT_SECS * 1000;
  twdt_config.idle_core_mask = 0;
  twdt_config.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&twdt_config) != ESP_OK) {
    esp_task_wdt_init(&twdt_config);
  }
  esp_task_wdt_add(NULL);

  Wire.begin(UEXT_I2C_SDA_PIN, UEXT_I2C_SCL_PIN);
  Wire.setTimeOut(50); // bound a wedged bus instead of blocking the loop

  Serial.println();
  Serial.printf("Relay test starting. Reset reason: %d. WDT %lus.\n",
                (int)esp_reset_reason(), WDT_TIMEOUT_SECS);
  uint8_t probe = modIo2WriteRaw(0);
  if (probe == 0) {
    Serial.println("MOD-IO2 detected on I2C 0x21, relays cleared.");
  } else {
    Serial.printf("WARNING: MOD-IO2 not responding on I2C 0x21 (%s) - check UEXT cable.\n",
                  i2cErrText(probe));
  }
  esp_task_wdt_reset();
}

void loop() {
  unsigned long cycleStart = millis();
  Serial.printf("--- cycle %lu (t=%lus) ---\n", (unsigned long)++cycleCount, millis() / 1000);

  for (int i = 0; i < num_evb_relays; i++) {
    unsigned long t0 = millis();
    Serial.printf("EVB relay %s (GPIO %d) ON\n", evb_relay_names[i], evb_relay_gpios[i]);
    digitalWrite(evb_relay_gpios[i], HIGH);
    holdFor(1000);
    digitalWrite(evb_relay_gpios[i], LOW);
    Serial.printf("  off after %lums\n", millis() - t0);
    esp_task_wdt_reset();
  }

  for (int i = 0; i < 2; i++) {
    unsigned long t0 = millis();
    Serial.printf("MOD-IO2 relay %d (%s) ON\n", i + 1, i == 0 ? "Humidity" : "Spare");
    if (!setModIo2Relays(1 << i, "on")) {
      Serial.println("  could not energize - skipping");
      continue;
    }
    holdFor(1000);
    // The write that matters: if this never lands the contact stays closed.
    if (!setModIo2Relays(0, "off")) {
      Serial.println("  *** RELAY MAY BE LATCHED CLOSED - I2C off-write failed ***");
    } else {
      Serial.printf("  off after %lums\n", millis() - t0);
    }
    esp_task_wdt_reset();
  }

  Serial.printf("--- cycle %lu done in %lums ---\n",
                (unsigned long)cycleCount, millis() - cycleStart);
  allRelaysOff();
  esp_task_wdt_reset();
}
