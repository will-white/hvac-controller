// Networking modules are not part of the native build (WiFi, TLS, HTTP and
// MQTT have no host shims). These stand-ins report "offline", which is a
// state the firmware must handle anyway.
#include "hvac_tests.h"
#include "mqtt.h"
#include "net_time.h"
#include "ota.h"
#include "weather.h"
#include "web.h"

void net_time_begin() {}
void net_time_poll() {}
bool isWifiConnected() { return false; }
bool isTimeValid() { return g_isTesting; } // same contract as net_time.cpp offline

void mqtt_begin() {}
void mqtt_poll() {}
bool mqtt_connected() { return false; }

void weather_poll() {}
void web_poll() {}

void ota_begin() {}
void ota_bootSelfTest(int) {}
void ota_poll() {}
void ota_requestCheck() {}
void ota_requestInstall() {}
void ota_setMode(OtaMode) {}
OtaMode ota_mode() { return OTA_OFF; }
const char* ota_statusText() { return "native build"; }
const char* ota_availableVersion() { return ""; }
bool ota_verifying() { return false; }
void ota_printStatus() {}
