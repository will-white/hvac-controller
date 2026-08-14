#ifndef RELAYS_H
#define RELAYS_H

// Relay hardware abstraction for ESP32-C6-EVB (GPIO relays) + MOD-IO2 (I2C
// relays). All callers use the logical relay indices from config.h
// (HEATER_RELAY, ...) — never raw GPIO numbers.
void initRelays();
void writeRelay(int relay, bool value);
bool readRelay(int relay);
int relays_modio2FailureCount(); // lifetime failed writes (health diagnostics)

#endif // RELAYS_H
