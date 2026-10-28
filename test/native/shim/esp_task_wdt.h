// Host stand-in: the task watchdog does nothing off-device.
#pragma once
#include <cstdint>
#include "esp_err.h"
typedef struct { uint32_t timeout_ms; uint32_t idle_core_mask; bool trigger_panic; } esp_task_wdt_config_t;
inline esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t*) { return ESP_OK; }
inline esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t*) { return ESP_OK; }
inline esp_err_t esp_task_wdt_add(void*) { return ESP_OK; }
inline esp_err_t esp_task_wdt_delete(void*) { return ESP_OK; }
inline esp_err_t esp_task_wdt_reset() { return ESP_OK; }
