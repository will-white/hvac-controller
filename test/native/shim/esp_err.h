// Host stand-in for esp_err.h (just the codes the firmware uses).
#pragma once
#include <cstdio>
#include <cstdlib>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_READ_ONLY 0x1107
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110

const char* esp_err_to_name(esp_err_t e);
#define ESP_ERROR_CHECK(x) do { esp_err_t e_ = (x); if (e_ != ESP_OK) { \
  fprintf(stderr, "ESP_ERROR_CHECK failed: %d at %s:%d\n", e_, __FILE__, __LINE__); abort(); } } while (0)
