#include <Arduino.h>
#include <Wire.h>
#include "config.h"

// Standalone hardware bring-up sketch for the relay stack:
//   - ESP32-C6-EVB onboard relays REL1-REL4 (GPIO driven)
//   - MOD-IO2 Rev.B relays 1-2 over UEXT I2C (address 0x21, command 0x40)
// Cycles each relay on for 1 s in turn. Watch the per-relay status LEDs.
// Reminder: MOD-IO2 relays only actuate with 12V DC on the MOD-IO2 barrel jack.

const int evb_relay_gpios[] = {10, 11, 22, 23};
const char* evb_relay_names[] = {"REL1/Heater", "REL2/Cooler", "REL3/Fan", "REL4/FreshAir"};
const int num_evb_relays = sizeof(evb_relay_gpios) / sizeof(evb_relay_gpios[0]);

bool setModIo2Relays(uint8_t mask) {
  Wire.beginTransmission(MODIO2_I2C_ADDRESS);
  Wire.write(0x40);
  Wire.write(mask & 0x03);
  return Wire.endTransmission() == 0;
}

void setup() {
  Serial.begin(115200);
  // USB-Serial-JTAG console: don't block forever waiting for a host.
  for (unsigned long t0 = millis(); !Serial && millis() - t0 < 3000; ) delay(10);

  for (int i = 0; i < num_evb_relays; i++) {
    pinMode(evb_relay_gpios[i], OUTPUT);
    digitalWrite(evb_relay_gpios[i], LOW);
  }

  Wire.begin(UEXT_I2C_SDA_PIN, UEXT_I2C_SCL_PIN);
  if (setModIo2Relays(0)) {
    Serial.println("MOD-IO2 detected on I2C 0x21.");
  } else {
    Serial.println("WARNING: MOD-IO2 not responding on I2C 0x21 (check UEXT cable).");
  }
}

void loop() {
  for (int i = 0; i < num_evb_relays; i++) {
    Serial.printf("EVB relay %s (GPIO %d) ON\n", evb_relay_names[i], evb_relay_gpios[i]);
    digitalWrite(evb_relay_gpios[i], HIGH);
    delay(1000);
    digitalWrite(evb_relay_gpios[i], LOW);
  }
  for (int i = 0; i < 2; i++) {
    Serial.printf("MOD-IO2 relay %d (%s) ON\n", i + 1, i == 0 ? "Humidity" : "Spare");
    if (!setModIo2Relays(1 << i)) Serial.println("  (I2C write failed)");
    delay(1000);
    setModIo2Relays(0);
  }
}
