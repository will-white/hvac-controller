// Host stand-in for NVS: an in-memory key/value store per namespace. It keeps
// its contents across "reboots" within one test process; native_nvsErase()
// wipes it (a factory reset).
#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

esp_err_t nvs_open(const char* ns, nvs_open_mode_t mode, nvs_handle_t* out);
void nvs_close(nvs_handle_t h);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_erase_key(nvs_handle_t h, const char* key);
esp_err_t nvs_set_str(nvs_handle_t h, const char* key, const char* value);
esp_err_t nvs_get_str(nvs_handle_t h, const char* key, char* out, size_t* len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char* key, const void* value, size_t len);
esp_err_t nvs_get_blob(nvs_handle_t h, const char* key, void* out, size_t* len);
esp_err_t nvs_set_u8(nvs_handle_t h, const char* key, uint8_t v);
esp_err_t nvs_get_u8(nvs_handle_t h, const char* key, uint8_t* v);
esp_err_t nvs_set_i8(nvs_handle_t h, const char* key, int8_t v);
esp_err_t nvs_get_i8(nvs_handle_t h, const char* key, int8_t* v);

void native_nvsErase();
