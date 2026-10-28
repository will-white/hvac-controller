#include "ota.h"
#include <cstdio>
#include <cstring>
#include "esp_app_desc.h"
#include "esp_app_format.h"

// The updater's pure helpers: no networking, no flash, so the native test
// build (test/native) compiles them without the HTTPS stack.

static bool jsonString(const char* json, const char* key, char* out, size_t len) {
  char pat[24];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char* p = strstr(json, pat);
  if (!p) return false;
  p += strlen(pat);
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (*p++ != ':') return false;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (*p++ != '"') return false;
  size_t n = 0;
  while (*p && *p != '"') {
    if (n + 1 >= len) return false;
    out[n++] = *p++;
  }
  if (*p != '"') return false;
  out[n] = '\0';
  return n > 0;
}

bool ota_parseManifest(const char* json, char* sha, size_t shaLen, char* ver, size_t verLen) {
  if (!json || !jsonString(json, "sha", sha, shaLen)) return false;
  if (strlen(sha) != 40) return false;
  for (const char* c = sha; *c; c++)
    if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'))) return false;
  if (!jsonString(json, "version", ver, verLen)) strlcpy(ver, sha, verLen > 8 ? 8 : verLen);
  return true;
}

bool ota_isAppImage(const uint8_t* buf, size_t len) {
  // esp_image_header_t (24 B) + first segment header (8 B), then the app
  // descriptor. A bootloader or merged factory image fails the magic check.
  if (!buf || len < 36 || buf[0] != ESP_IMAGE_HEADER_MAGIC) return false;
  uint16_t chip = (uint16_t)(buf[12] | (buf[13] << 8));
  if (chip != ESP_CHIP_ID_ESP32C6) return false;
  uint32_t magic = (uint32_t)buf[32] | ((uint32_t)buf[33] << 8) |
                   ((uint32_t)buf[34] << 16) | ((uint32_t)buf[35] << 24);
  return magic == ESP_APP_DESC_MAGIC_WORD;
}

const char* ota_modeName(OtaMode m) {
  switch (m) { case OTA_AUTO: return "auto"; case OTA_NOTIFY: return "notify"; default: return "off"; }
}

bool ota_parseMode(const char* s, OtaMode* out) {
  if (!s) return false;
  if (!strcmp(s, "auto")) *out = OTA_AUTO;
  else if (!strcmp(s, "notify")) *out = OTA_NOTIFY;
  else if (!strcmp(s, "off")) *out = OTA_OFF;
  else return false;
  return true;
}
