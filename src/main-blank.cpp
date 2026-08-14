#include <Arduino.h>
#include <Wire.h>
#include "config.h"

// Standalone "all quiet" sketch — the counterpart to main-relay-test.cpp.
// Flash this after a relay bring-up test to stop the test cycling: it drives
// every relay off (EVB REL1-REL4 on GPIO, MOD-IO2 relays 1-2 over UEXT I2C)
// and then does nothing else. No WiFi, no MQTT, no control logic.
// Reminder: MOD-IO2 relays only actuate with 12V DC on the MOD-IO2 barrel jack.

static const int evb_relay_gpios[] = {10, 11, 22, 23};
static const int num_evb_relays = sizeof(evb_relay_gpios) / sizeof(evb_relay_gpios[0]);

static bool setModIo2Relays(uint8_t mask) {
  Wire.beginTransmission(MODIO2_I2C_ADDRESS);
  Wire.write(0x40);
  Wire.write(mask & 0x03);
  return Wire.endTransmission() == 0;
}

void setup() {
  Serial.begin(115200);
  // With no host draining the CDC, blocking writes would stall the loop.
  Serial.setTxTimeoutMs(0);
  // USB-Serial-JTAG console: don't block forever waiting for a host.
  for (unsigned long t0 = millis(); !Serial && millis() - t0 < 3000; ) delay(10);

  for (int i = 0; i < num_evb_relays; i++) {
    pinMode(evb_relay_gpios[i], OUTPUT);
    digitalWrite(evb_relay_gpios[i], LOW);
  }

  Wire.begin(UEXT_I2C_SDA_PIN, UEXT_I2C_SCL_PIN);
  // Retry briefly: the MOD-IO2 may still be coming up on its own 12V rail.
  bool modio2Off = false;
  for (int attempt = 0; attempt < 5 && !modio2Off; attempt++) {
    modio2Off = setModIo2Relays(0);
    if (!modio2Off) delay(100);
  }

  Serial.println();
  Serial.println("Blank sketch: all relays commanded OFF, no control logic running.");
  Serial.printf("  EVB REL1-REL4 (GPIO 10/11/22/23): off\n");
  Serial.printf("  MOD-IO2 relays 1-2: %s\n",
                modio2Off ? "off" : "NOT RESPONDING on I2C 0x21 (check UEXT cable)");
}

void loop() {
  // Heartbeat only, so it's obvious which firmware is on the board.
  static unsigned long last = 0;
  if (millis() - last >= 10000) {
    last = millis();
    Serial.println("blank: idle, relays off");
  }
  delay(100);
}
