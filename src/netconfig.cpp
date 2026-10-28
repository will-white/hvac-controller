#include "netconfig.h"
#include <Arduino.h>
#include <cstdlib>
#include <cstring>
#include "nvs.h"

// The only file that reads secrets.h.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""
#endif
#ifndef MQTT_HOST
#define MQTT_HOST ""
#define MQTT_PORT 1883
#define MQTT_USER ""
#define MQTT_PASS ""
#endif
#ifndef Z2M_INDOOR_TOPIC
#define Z2M_INDOOR_TOPIC ""
#endif
#ifndef Z2M_OUTDOOR_TOPIC
#define Z2M_OUTDOOR_TOPIC ""
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif
#ifndef WEATHER_LATITUDE
#define WEATHER_LATITUDE  0.0
#define WEATHER_LONGITUDE 0.0
#endif

static NetConfig cfg;
// Numeric fields are stored as strings like the rest, then parsed.
static char portStr[8];
static char latStr[16];
static char lonStr[16];

struct Field {
  const char* key; // also the NVS key (<= 15 chars)
  char* buf;
  size_t size;
  bool secret;
};

static Field fields[] = {
  {"wifi_ssid",    cfg.wifiSsid,    sizeof(cfg.wifiSsid),    false},
  {"wifi_pass",    cfg.wifiPass,    sizeof(cfg.wifiPass),    true},
  {"mqtt_host",    cfg.mqttHost,    sizeof(cfg.mqttHost),    false},
  {"mqtt_port",    portStr,         sizeof(portStr),         false},
  {"mqtt_user",    cfg.mqttUser,    sizeof(cfg.mqttUser),    false},
  {"mqtt_pass",    cfg.mqttPass,    sizeof(cfg.mqttPass),    true},
  {"z2m_indoor",   cfg.z2mIndoor,   sizeof(cfg.z2mIndoor),   false},
  {"z2m_outdoor",  cfg.z2mOutdoor,  sizeof(cfg.z2mOutdoor),  false},
  {"ota_password", cfg.otaPassword, sizeof(cfg.otaPassword), true},
  {"lat",          latStr,          sizeof(latStr),          false},
  {"lon",          lonStr,          sizeof(lonStr),          false},
};
static const int FIELD_COUNT = sizeof(fields) / sizeof(fields[0]);

static void parseNumbers() {
  int port = atoi(portStr);
  cfg.mqttPort = (port > 0 && port < 65536) ? (uint16_t)port : 1883;
  cfg.latitude = (float)atof(latStr);
  cfg.longitude = (float)atof(lonStr);
}

static void writeIfChanged(nvs_handle_t h, const Field& f, const char* value) {
  char cur[128] = "";
  size_t len = sizeof(cur);
  if (nvs_get_str(h, f.key, cur, &len) == ESP_OK && strcmp(cur, value) == 0) return;
  nvs_set_str(h, f.key, value);
}

void netcfg_begin() {
  memset(&cfg, 0, sizeof(cfg));
  nvs_handle_t h;
  if (nvs_open("netcfg", NVS_READWRITE, &h) != ESP_OK) {
    Serial.println("netcfg: NVS unavailable - network features disabled.");
    parseNumbers();
    return;
  }

  if (WIFI_SSID[0] != '\0') {
    // This build carries secrets.h: it is authoritative, persist it.
    char port[8], lat[16], lon[16];
    snprintf(port, sizeof(port), "%d", (int)MQTT_PORT);
    snprintf(lat, sizeof(lat), "%.4f", (double)WEATHER_LATITUDE);
    snprintf(lon, sizeof(lon), "%.4f", (double)WEATHER_LONGITUDE);
    const char* compiled[FIELD_COUNT] = {
      WIFI_SSID, WIFI_PASSWORD, MQTT_HOST, port, MQTT_USER, MQTT_PASS,
      Z2M_INDOOR_TOPIC, Z2M_OUTDOOR_TOPIC, OTA_PASSWORD, lat, lon,
    };
    for (int i = 0; i < FIELD_COUNT; i++) writeIfChanged(h, fields[i], compiled[i]);
    nvs_commit(h);
  }

  for (int i = 0; i < FIELD_COUNT; i++) {
    size_t len = fields[i].size;
    if (nvs_get_str(h, fields[i].key, fields[i].buf, &len) != ESP_OK) fields[i].buf[0] = '\0';
  }
  nvs_close(h);
  parseNumbers();
}

const NetConfig& netcfg() { return cfg; }

bool netcfg_set(const char* key, const char* value) {
  for (int i = 0; i < FIELD_COUNT; i++) {
    if (strcmp(key, fields[i].key) != 0) continue;
    if (strlen(value) >= fields[i].size) return false;
    nvs_handle_t h;
    if (nvs_open("netcfg", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, key, value) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) {
      strlcpy(fields[i].buf, value, fields[i].size);
      parseNumbers();
    }
    return ok;
  }
  return false;
}

void netcfg_print() {
  Serial.printf("Network settings (NVS%s):\n", WIFI_SSID[0] ? ", seeded from this build's secrets.h" : "");
  for (int i = 0; i < FIELD_COUNT; i++) {
    const char* v = fields[i].buf;
    Serial.printf("  %-13s %s\n", fields[i].key,
                  !v[0] ? "(empty)" : fields[i].secret ? "********" : v);
  }
}

const char* netcfg_keys() {
  return "wifi_ssid wifi_pass mqtt_host mqtt_port mqtt_user mqtt_pass "
         "z2m_indoor z2m_outdoor ota_password lat lon";
}
