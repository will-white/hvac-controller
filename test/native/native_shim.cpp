// Definitions behind the host shims in shim/: fake clock, GPIO, Serial, the
// in-memory NVS, and the absent I2C bus / filesystem.
#include <Arduino.h>
#include <LittleFS.h>
#include <Wire.h>
#include <map>
#include <string>
#include <vector>
#include "esp_system.h"
#include "nvs_flash.h"

HostSerial Serial;
TwoWire Wire;
LittleFSFS LittleFS;

static unsigned long g_nativeMillis = 1000;
unsigned long millis() { return g_nativeMillis; }
void delay(unsigned long ms) { g_nativeMillis += ms; }
void native_advanceMillis(unsigned long ms) { g_nativeMillis += ms; }

static uint8_t g_gpio[64];
void pinMode(uint8_t, uint8_t) {}
void digitalWrite(uint8_t pin, uint8_t val) { if (pin < 64) g_gpio[pin] = val; }
int digitalRead(uint8_t pin) { return pin < 64 ? g_gpio[pin] : LOW; }

void esp_restart() {
  fprintf(stderr, "esp_restart() called in the native build\n");
  std::exit(70);
}

esp_reset_reason_t esp_reset_reason() { return ESP_RST_POWERON; }

size_t native_strlcpy(char* dst, const char* src, size_t size) {
  size_t n = strlen(src);
  if (size) {
    size_t k = n < size - 1 ? n : size - 1;
    memcpy(dst, src, k);
    dst[k] = '\0';
  }
  return n;
}

int HostSerial::printf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vprintf(fmt, ap);
  va_end(ap);
  return n;
}

const char* esp_err_to_name(esp_err_t e) {
  switch (e) {
    case ESP_OK: return "ESP_OK";
    case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_READ_ONLY: return "ESP_ERR_NVS_READ_ONLY";
    case ESP_ERR_NVS_INVALID_LENGTH: return "ESP_ERR_NVS_INVALID_LENGTH";
    default: return "ESP_FAIL";
  }
}

// ---- NVS ----
struct Handle { std::string ns; bool writable; };
static std::map<std::string, std::map<std::string, std::vector<uint8_t>>> g_nvs;
static std::map<nvs_handle_t, Handle> g_handles;
static nvs_handle_t g_nextHandle = 1;

void native_nvsErase() { g_nvs.clear(); }
esp_err_t nvs_flash_init() { return ESP_OK; }
esp_err_t nvs_flash_erase() { native_nvsErase(); return ESP_OK; }

esp_err_t nvs_open(const char* ns, nvs_open_mode_t mode, nvs_handle_t* out) {
  if (mode == NVS_READONLY && !g_nvs.count(ns)) return ESP_ERR_NVS_NOT_FOUND;
  if (mode == NVS_READWRITE) g_nvs[ns]; // creates the namespace, as on device
  *out = g_nextHandle++;
  g_handles[*out] = Handle{ns, mode == NVS_READWRITE};
  return ESP_OK;
}
void nvs_close(nvs_handle_t h) { g_handles.erase(h); }
esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }

static esp_err_t put(nvs_handle_t h, const char* key, const void* p, size_t n) {
  auto it = g_handles.find(h);
  if (it == g_handles.end()) return ESP_ERR_INVALID_ARG;
  if (!it->second.writable) return ESP_ERR_NVS_READ_ONLY;
  const uint8_t* b = static_cast<const uint8_t*>(p);
  g_nvs[it->second.ns][key] = std::vector<uint8_t>(b, b + n);
  return ESP_OK;
}

static const std::vector<uint8_t>* get(nvs_handle_t h, const char* key) {
  auto it = g_handles.find(h);
  if (it == g_handles.end()) return nullptr;
  auto& ns = g_nvs[it->second.ns];
  auto kv = ns.find(key);
  return kv == ns.end() ? nullptr : &kv->second;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char* key) {
  auto it = g_handles.find(h);
  if (it == g_handles.end()) return ESP_ERR_INVALID_ARG;
  return g_nvs[it->second.ns].erase(key) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char* key, const char* v) { return put(h, key, v, strlen(v) + 1); }
esp_err_t nvs_get_str(nvs_handle_t h, const char* key, char* out, size_t* len) {
  const std::vector<uint8_t>* v = get(h, key);
  if (!v) return ESP_ERR_NVS_NOT_FOUND;
  if (!out) { *len = v->size(); return ESP_OK; }
  if (*len < v->size()) return ESP_ERR_NVS_INVALID_LENGTH;
  memcpy(out, v->data(), v->size());
  *len = v->size();
  return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char* key, const void* v, size_t n) { return put(h, key, v, n); }
esp_err_t nvs_get_blob(nvs_handle_t h, const char* key, void* out, size_t* len) {
  const std::vector<uint8_t>* v = get(h, key);
  if (!v) return ESP_ERR_NVS_NOT_FOUND;
  if (!out) { *len = v->size(); return ESP_OK; }
  if (*len < v->size()) return ESP_ERR_NVS_INVALID_LENGTH;
  memcpy(out, v->data(), v->size());
  *len = v->size();
  return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char* key, uint8_t v) { return put(h, key, &v, 1); }
esp_err_t nvs_get_u8(nvs_handle_t h, const char* key, uint8_t* v) {
  const std::vector<uint8_t>* b = get(h, key);
  if (!b || b->size() != 1) return ESP_ERR_NVS_NOT_FOUND;
  *v = (*b)[0];
  return ESP_OK;
}
esp_err_t nvs_set_i8(nvs_handle_t h, const char* key, int8_t v) { return put(h, key, &v, 1); }
esp_err_t nvs_get_i8(nvs_handle_t h, const char* key, int8_t* v) {
  const std::vector<uint8_t>* b = get(h, key);
  if (!b || b->size() != 1) return ESP_ERR_NVS_NOT_FOUND;
  *v = (int8_t)(*b)[0];
  return ESP_OK;
}
