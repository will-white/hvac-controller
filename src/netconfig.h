#ifndef NETCONFIG_H
#define NETCONFIG_H

#include <stdint.h>

// Network credentials and endpoints, persisted in NVS (namespace "netcfg").
//
// Firmware published by CI carries no secrets.h, so credentials cannot live
// only in the binary or a WiFi update would boot offline. Rule: a build whose
// secrets.h sets WIFI_SSID is authoritative — every field it defines is written
// to NVS at boot (only where it differs, to spare flash). A build without one
// uses whatever NVS holds. The console `net set` edits NVS directly; changes
// apply after a reboot.
struct NetConfig {
  char wifiSsid[33];
  char wifiPass[65];
  char mqttHost[64];
  uint16_t mqttPort;
  char mqttUser[33];
  char mqttPass[65];
  char z2mIndoor[96];
  char z2mOutdoor[96];
  char otaPassword[33];
  float latitude;   // 0/0 = weather feed disabled
  float longitude;
};

void netcfg_begin();                       // after nvs_flash_init(), before networking
const NetConfig& netcfg();
bool netcfg_set(const char* key, const char* value); // persists; false = unknown key / too long
void netcfg_print();                       // console dump, passwords masked
const char* netcfg_keys();                 // "wifi_ssid wifi_pass ..." for help text

#endif // NETCONFIG_H
