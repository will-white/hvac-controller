#include <Arduino.h>

// Define the relay pins
const int relay_pins[] = {2, 4, 5, 12, 13, 14, 15, 16};
const int num_relays = sizeof(relay_pins) / sizeof(relay_pins[0]);

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    ; // wait for serial port to connect. Needed for native USB
  }

  for (int i = 0; i < num_relays; i++) {
    pinMode(relay_pins[i], OUTPUT);
    digitalWrite(relay_pins[i], LOW);
  }
}

void loop() {
  for (int i = 0; i < num_relays; i++) {
    Serial.print("Turning on relay ");
    Serial.println(i);
    digitalWrite(relay_pins[i], HIGH);
    delay(1000);
    digitalWrite(relay_pins[i], LOW);
  }
}