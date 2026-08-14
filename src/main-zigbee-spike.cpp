// Phase 6 feasibility spike (standalone sketch, `zigbee_spike` env): can the
// C6 run as a native Zigbee COORDINATOR while WiFi stays usable on the same
// radio? Forms a network, opens it for joining, keeps a thermostat endpoint
// ready to receive temperature/humidity reports (the Aqara use case), and
// exercises WiFi scans in a loop to probe RF coexistence.
//
// Findings live in ROADMAP.md (Phase 6, Aqara probe path B).
// Built twice: `zigbee_spike` (full) and `zigbee_spike_baseline` (same sketch
// without the Zigbee stack) - the flash-size difference IS the ZBOSS cost.
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#ifdef ZIGBEE_MODE_ZCZR
#include "Zigbee.h"

static ZigbeeThermostat zbThermostat(1);

static void onTemp(float tempC) {
  Serial.printf("[zb] temperature report: %.2fC\n", tempC);
}
static void onHumidity(float rh) {
  Serial.printf("[zb] humidity report: %.1f%%\n", rh);
}
#endif

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0); // never block on an undrained CDC ring
  for (unsigned long t0 = millis(); !Serial && millis() - t0 < 3000; ) delay(10);
  Serial.println("\n=== Zigbee coordinator + WiFi coexistence spike ===");

#ifdef ZIGBEE_MODE_ZCZR
  zbThermostat.onTempReceive(onTemp);
  zbThermostat.onHumidityReceive(onHumidity);
  Zigbee.addEndpoint(&zbThermostat);

  Serial.println("Starting Zigbee stack as COORDINATOR (fresh network)...");
  if (!Zigbee.begin(ZIGBEE_COORDINATOR, /*erase_nvs=*/true)) {
    Serial.println("RESULT: Zigbee.begin() FAILED");
  } else {
    Serial.println("RESULT: Zigbee coordinator started; network open for joining 180s.");
    Zigbee.openNetwork(180);
  }
#else
  Serial.println("(baseline build: Zigbee stack excluded)");
  // RF sanity check: broadcast our own AP; if the host PC can see/join it the
  // C6 radio works and any scan blindness is on the bench-AP side.
  WiFi.mode(WIFI_AP_STA);
  bool apOk = WiFi.softAP("c6-spike-ap", "spike-test-1234");
  Serial.printf("SoftAP 'c6-spike-ap': %s (ch %d)\n", apOk ? "up" : "FAILED", (int)WiFi.channel());
#endif

  // Bench AP: the Windows mobile hotspot the spike harness stands up.
  WiFi.mode(WIFI_STA);
  // Clean scan first (a pending association makes scanNetworks fail with -2).
  int n = WiFi.scanNetworks();
  Serial.printf("Boot scan: %d network(s)\n", n);
  for (int i = 0; i < n && i < 8; i++)
    Serial.printf("  %s ch%d %ddBm\n", WiFi.SSID(i).c_str(), (int)WiFi.channel(i), (int)WiFi.RSSI(i));
  WiFi.scanDelete();
  WiFi.begin("hvac-spike", "spike-test-1234");
  Serial.println("WiFi STA associating to 'hvac-spike'...");
}

void loop() {
  static unsigned long lastMs = 0;
  static int iter = 0;
  if (millis() - lastMs > 15000) {
    lastMs = millis();
    iter++;
#ifdef ZIGBEE_MODE_ZCZR
    bool zbUp = Zigbee.started();
#else
    bool zbUp = false;
#endif
    wl_status_t st = WiFi.status();
    Serial.printf("[%4lus] zb:%d | wifi status:%d ip:%s rssi:%d",
                  millis() / 1000, zbUp, (int)st, WiFi.localIP().toString().c_str(),
                  st == WL_CONNECTED ? (int)WiFi.RSSI() : 0);
#ifndef ZIGBEE_MODE_ZCZR
    Serial.printf(" | AP clients:%d", (int)WiFi.softAPgetStationNum());
#endif
    Serial.println();

    // Real traffic through the AP while the coordinator radio is live.
    if (st == WL_CONNECTED) {
      HTTPClient http;
      http.setConnectTimeout(4000);
      unsigned long t0 = millis();
      if (http.begin("http://www.msftconnecttest.com/connecttest.txt")) {
        int code = http.GET();
        Serial.printf("       HTTP GET: code %d in %lums\n", code, millis() - t0);
        http.end();
      }
    }

    if (iter % 4 == 3) { // occasional scan (disassociates briefly; that's fine)
      unsigned long t0 = millis();
      int n = WiFi.scanNetworks();
      Serial.printf("       WiFi scan: %d network(s) in %lums", n, millis() - t0);
      if (n > 0) Serial.printf(" (strongest: %s %ddBm)", WiFi.SSID(0).c_str(), (int)WiFi.RSSI(0));
      Serial.println();
      WiFi.scanDelete();
    }
  }
  delay(50);
}
