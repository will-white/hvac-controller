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
#include <esp_wifi.h>
#ifdef ZIGBEE_MODE_ZCZR
#include "Zigbee.h"

static ZigbeeThermostat zbThermostat(1);

static void onTemp(float tempC) {
  Serial.printf("[zb] temperature report: %.2fC\n", tempC);
}
static void onHumidity(float rh) {
  Serial.printf("[zb] humidity report: %.1f%%\n", rh);
}
#else
#include <esp_phy_init.h>
// From esp_private/wifi.h (not on the Arduino include path): blob log control.
extern "C" esp_err_t esp_wifi_internal_set_log_level(uint32_t level);
static volatile uint32_t g_probeCount = 0; // probe requests heard (board RX proof)
// Raw RX proof, beneath all AP/assoc logic: promiscuous frame counters on our
// channel. Anything on the air nearby (a joining phone, other APs' beacons)
// must move these if the receive path works at all.
static volatile uint32_t g_rxFrames = 0, g_rxMgmt = 0, g_rxBeacons = 0, g_rxData = 0;
// Join frames addressed TO OUR MAC + RSSI stats: distinguishes "frames never
// arrive" (RF) from "frames arrive and the AP stack ignores them" (software).
static volatile uint32_t g_authToUs = 0, g_assocToUs = 0, g_probeToUs = 0;
static volatile int g_rssiLast = 0, g_rssiMin = 0, g_rssiMax = -127, g_rssiToUsLast = 0;
static uint8_t g_apMac[6] = {0};

// Site-survey table: every distinct SSID heard in beacons, with channel+RSSI.
struct ApSeen { char ssid[33]; int8_t rssi; uint8_t ch; uint32_t count; };
static ApSeen g_aps[16];
static volatile int g_apCount = 0;

static void recordBeacon(const wifi_promiscuous_pkt_t* p) {
  // Beacon: 24B mgmt header + 12B fixed params, then IEs; IE 0 = SSID.
  if (p->rx_ctrl.sig_len < 40 || p->payload[36] != 0) return;
  uint8_t len = p->payload[37];
  if (len > 32) return;
  char ssid[33];
  if (len == 0) { strcpy(ssid, "<hidden>"); }
  else { memcpy(ssid, p->payload + 38, len); ssid[len] = '\0'; }
  for (int i = 0; i < g_apCount; i++) {
    if (strcmp(g_aps[i].ssid, ssid) == 0) {
      g_aps[i].count++;
      if (p->rx_ctrl.rssi > g_aps[i].rssi) { g_aps[i].rssi = p->rx_ctrl.rssi; g_aps[i].ch = p->rx_ctrl.channel; }
      return;
    }
  }
  if (g_apCount < 16) {
    ApSeen& a = g_aps[g_apCount];
    strcpy(a.ssid, ssid);
    a.rssi = p->rx_ctrl.rssi;
    a.ch = p->rx_ctrl.channel;
    a.count = 1;
    g_apCount = g_apCount + 1;
  }
}

static void snifferCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  int rssi = p->rx_ctrl.rssi;
  g_rxFrames++;
  g_rssiLast = rssi;
  if (rssi < g_rssiMin) g_rssiMin = rssi;
  if (rssi > g_rssiMax) g_rssiMax = rssi;
  if (type == WIFI_PKT_MGMT) {
    g_rxMgmt++;
    uint8_t subtype = p->payload[0] & 0xF0;
    if (subtype == 0x80) { g_rxBeacons++; recordBeacon(p); }
    // addr1 (destination) is payload bytes 4..9
    bool toUs = memcmp((const void*)(p->payload + 4), g_apMac, 6) == 0;
    if (toUs) {
      g_rssiToUsLast = rssi;
      if (subtype == 0xB0) g_authToUs++;                          // authentication
      else if (subtype == 0x00 || subtype == 0x20) g_assocToUs++; // (re)assoc request
      else if (subtype == 0x40) g_probeToUs++;                    // directed probe req
    }
  } else if (type == WIFI_PKT_DATA) {
    g_rxData++;
  }
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
  // Corrupted PHY calibration is the one persistent software state that can
  // ruin RX sensitivity while TX looks fine - wipe it so WiFi start performs
  // a full fresh calibration.
  esp_err_t calErr = esp_phy_erase_cal_data_in_nvs();
  Serial.printf("PHY calibration data erased (%s) - full recalibration on radio start\n",
                esp_err_to_name(calErr));
  // RF sanity check: broadcast our own AP - AP-ONLY, no STA. In AP_STA mode the
  // STA's connect attempts drag the shared radio across channels and disrupt
  // beaconing; a pure AP keeps beacons on one channel continuously.
  // OPEN network (no WPA): removes the 4-way handshake so a join exercises
  // pure RF + assoc + DHCP. Event log shows how far each attempt gets.
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
      case ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED:
        g_probeCount++;
        break;
      case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        Serial.println("[evt] >> STA ASSOCIATED to the hotspot");
        break;
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        Serial.println("[evt] >> GOT IP - fully joined");
        break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        Serial.printf("[evt] >> STA DISCONNECTED/FAILED (reason %d)\n",
                      (int)info.wifi_sta_disconnected.reason);
        break;
      case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
        Serial.printf("[evt] >> STA ASSOCIATED (aid %d)", info.wifi_ap_staconnected.aid);
        wifi_sta_list_t stas;
        if (esp_wifi_ap_get_sta_list(&stas) == ESP_OK && stas.num > 0) {
          Serial.printf(" rssi %ddBm", (int)stas.sta[stas.num - 1].rssi);
        }
        Serial.println();
        break;
      }
      case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
        Serial.printf("[evt] >> STA DISCONNECTED (reason %d)\n", info.wifi_ap_stadisconnected.reason);
        break;
      case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
        Serial.println("[evt] >> DHCP lease issued - client fully joined");
        break;
      default:
        Serial.printf("[evt] wifi event %d\n", (int)event);
        break;
    }
  });
  // EVERYTHING-AT-ONCE: AP mode is the only mode where RX has provably worked,
  // so run the AP, the SSID-decoding sniffer, AND a STA join attempt together
  // (AP_STA). One window answers: is the bench hotspot really beaconing (and
  // on what channel), what channel is our radio actually on (per-frame), and
  // does a join succeed from the mode where the receiver demonstrably lives.
  // BEACON-RATE MEASUREMENT: pure AP mode parks the radio on ch6 with no STA
  // scans dragging it off-channel. 'Bogey' (a known live AP on ch6, ~-66dBm)
  // transmits ~10 beacons/s; a healthy parked receiver must catch ~all of
  // them. The catch-rate is the verdict: ~100/10s = RX fine (failures are in
  // radio scheduling / scan paths); ~0 = frames lost at the RF level.
  // Channel 6: GWN-Cloud beacons here at -53dBm - a strong reference source
  // for the per-boot catch-rate test (healthy radio: ~100 beacons / 10 s).
  WiFi.mode(WIFI_AP);
  esp_wifi_set_ps(WIFI_PS_NONE);
  bool apOk = WiFi.softAP("c6-spike-ap", "spike-test-1234", 6, 0, 4);
  esp_wifi_set_promiscuous_rx_cb(&snifferCb);
  wifi_promiscuous_filter_t filt = {};
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous(true);
  Serial.printf("AP %s (ch6, parked) + beacon-rate sniffer\n", apOk ? "up" : "FAILED");
  return; // reporting handled in loop()
#endif

  // Bench AP: a phone hotspot named 'hvac-spike' (pw spike-test-1234).
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
#ifndef ZIGBEE_MODE_ZCZR
  // Survey build: hop channels, report the table every 10s.
  // RX-health boot gate (workaround prototype): if the receiver drew a losing
  // boot (near-zero beacons on a channel known to carry a -53dBm AP), reboot
  // and re-roll. A winning boot proceeds - proving whether "reboot until the
  // radio wakes up" can unblock WiFi on this unit at zero cost.
  static bool gateDecided = false;
  if (!gateDecided && millis() > 15000) {
    gateDecided = true;
    if (g_rxBeacons < 20) {
      Serial.printf("RX-GATE: losing boot (%lu beacons in 15s) - rebooting to re-roll\n",
                    (unsigned long)g_rxBeacons);
      Serial.flush();
      delay(100);
      esp_restart();
    }
    Serial.printf("RX-GATE: WINNING BOOT (%lu beacons in 15s) - holding to observe stability\n",
                  (unsigned long)g_rxBeacons);
  }
  static unsigned long lastReportMs = 0;
  if (millis() - lastReportMs > 10000) {
    lastReportMs = millis();
    Serial.printf("[%4lus] AP clients:%d | %lu beacon(s), %d SSID(s) | TO-US auth:%lu assoc:%lu probe:%lu rssi:%d\n",
                  millis() / 1000, (int)WiFi.softAPgetStationNum(),
                  (unsigned long)g_rxBeacons, (int)g_apCount,
                  (unsigned long)g_authToUs, (unsigned long)g_assocToUs,
                  (unsigned long)g_probeToUs, g_rssiToUsLast);
    for (int i = 0; i < g_apCount; i++) {
      Serial.printf("   %-32s ch%-2d best %4ddBm  %lu beacon(s)\n",
                    g_aps[i].ssid, (int)g_aps[i].ch, (int)g_aps[i].rssi, (unsigned long)g_aps[i].count);
    }
  }
  delay(20);
  return;
#endif
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
