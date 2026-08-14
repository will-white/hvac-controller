#include "ota.h"
#include <Arduino.h>
#include <ArduinoOTA.h>
#include "esp_task_wdt.h"
#include "config.h"
#include "events.h"
#include "health.h"
#include "main.h"
#include "net_time.h"
#include "relays.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif

static bool otaStarted = false;

void ota_poll() {
  if (OTA_PASSWORD[0] == '\0') return;
  if (!isWifiConnected()) return;

  if (!otaStarted) {
    ArduinoOTA.setHostname("hvac-controller");
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
      // Never reflash with equipment energized, and don't lose pending data.
      for (int i = 0; i < RELAY_COUNT; i++) writeRelay(i, LOW);
      if (performanceDirty) savePerformance();
      health_flushIfDirty();
      logEvent("OTA update starting - relays off");
      // Flash writes stall the loop far past the WDT window.
      esp_task_wdt_delete(NULL);
    });
    ArduinoOTA.onError([](ota_error_t e) {
      logEvent("OTA error %d", (int)e);
      esp_task_wdt_add(NULL); // resume normal watchdog coverage
    });
    ArduinoOTA.begin();
    otaStarted = true;
    logEvent("OTA ready (hostname hvac-controller)");
  }
  ArduinoOTA.handle();
}
