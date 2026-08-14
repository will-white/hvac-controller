#include "net_time.h"
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "config.h"
#include "events.h"
#include "hvac_tests.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""
#endif

static bool credentialsPresent = false;
static unsigned long lastAttemptMs = 0;
static bool wasConnected = false;

void net_time_begin() {
  credentialsPresent = (WIFI_SSID[0] != '\0');
  if (!credentialsPresent) {
    Serial.println("No WiFi credentials (src/secrets.h) - running offline, schedule disabled until time is known.");
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
  lastAttemptMs = millis();
}

void net_time_poll() {
  if (!credentialsPresent) return;
  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected && !wasConnected) {
    logEvent("wifi connected, rssi %d", (int)WiFi.RSSI());
    // (Re-)arm SNTP after every association.
    configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
  } else if (!connected && wasConnected) {
    logEvent("wifi disconnected");
  } else if (!connected && millis() - lastAttemptMs > WIFI_RETRY_INTERVAL_MS) {
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastAttemptMs = millis();
  }
  wasConnected = connected;
}

bool isWifiConnected() {
  return credentialsPresent && WiFi.status() == WL_CONNECTED;
}

bool isTimeValid() {
  if (g_isTesting) return true; // boot tests drive time through g_mockTime
  // Anything before 2020 means SNTP has not delivered yet.
  return time(nullptr) > 1577836800;
}
