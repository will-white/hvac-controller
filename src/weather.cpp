#include "weather.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include "config.h"
#include "events.h"
#include "net_time.h"
#include "netconfig.h"
#include "sensors.h"

static unsigned long lastFetchMs = 0;
static bool everFetched = false;
static bool lastFailed = false;

void weather_poll() {
  if (netcfg().latitude == 0.0f && netcfg().longitude == 0.0f) return; // not configured
  if (!isWifiConnected()) return;
  unsigned long now = millis();
  if (everFetched && now - lastFetchMs < WEATHER_POLL_MINS * 60000UL) return;
  if (!everFetched && now < 15000) return; // let WiFi settle after boot
  lastFetchMs = now;
  everFetched = true;

  char url[192];
  snprintf(url, sizeof(url),
    "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
    "&current=temperature_2m&temperature_unit=fahrenheit",
    (double)netcfg().latitude, (double)netcfg().longitude);

  // Blocking fetch, bounded well under the WDT window.
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(3000);
  if (!http.begin(url)) return;
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    // "temperature_2m" appears twice (units block first) — anchor on "current".
    int cur = body.indexOf("\"current\":");
    int idx = cur >= 0 ? body.indexOf("\"temperature_2m\":", cur) : -1;
    if (idx >= 0) {
      sensors_injectOutdoorWeather(body.substring(idx + 17).toFloat());
      if (lastFailed) logEvent("weather fetch recovered");
      lastFailed = false;
    }
  } else {
    if (!lastFailed) logEvent("weather fetch failed (%d)", code);
    lastFailed = true;
  }
  http.end();
}
