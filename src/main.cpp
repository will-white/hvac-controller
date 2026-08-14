#include <Arduino.h>
#include <cmath>
#include <time.h>
#include "config.h"
#include "main.h"
#include "console.h"
#include "events.h"
#include "health.h"
#include "hvac_logic.h"
#include "hvac_tests.h"
#include "inputs.h"
#include "learning.h"
#include "mqtt.h"
#include "net_time.h"
#include "ota.h"
#include "relays.h"
#include "sensors.h"
#include "setpoints.h"
#include "status_led.h"
#include "utils.h"
#include "weather.h"
#include "web.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_task_wdt.h"

// -- Global variables for the main application --
Schedule programSchedule;
HvacPerformance performance;
TempUnit tempUnit = DEFAULT_TEMP_UNIT;
SystemMode systemMode = DEFAULT_SYSTEM_MODE;
bool vacationModeActive = false;
bool systemInFaultState = false;
bool systemLockedOut = false; // Shared with logic module

ResolvedTargets lastResolved = { NO_TIME_FALLBACK_HEAT_F, NO_TIME_FALLBACK_COOL_F,
                                 FAN_AUTO, DEFAULT_SYSTEM_MODE, SRC_NO_TIME_FALLBACK };
float lastIndoorF = NAN;
bool performanceDirty = false;
char g_safetyReason[64] = "";

// =================================================================
// ==              HARDWARE ABSTRACTION & UTILITIES               ==
// =================================================================
// Relay HAL lives in relays.cpp, sensors in sensors.cpp. Control-loop time is
// the console's virtual clock so `sim speed` can time-warp cycle timers.
unsigned long currentTime() { return g_isTesting ? g_mockMillis : console_virtualMillis(); }

// delay() does not feed the task watchdog of a subscribed task; any wait that
// can exceed WDT_TIMEOUT_SECS must go through this instead. Real time only —
// never called from the mock-time test paths.
void safeDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    esp_task_wdt_reset();
    delay(50);
  }
}

// =================================================================
// ==                  NVS SAVE/LOAD FUNCTIONS                    ==
// =================================================================
// User settings + schedule. Called on actual changes (mode/hold/schedule
// edits), NOT per-cycle — keep it that way for flash wear.
void saveSettings() {
    if (g_isTesting) return; // never persist state mutated by the boot-time self-tests
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("hvac_cfg", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) { Serial.printf("Error (%s) opening NVS handle for writing!\n", esp_err_to_name(err)); return; }

    nvs_set_u8(my_handle, "schema", SETTINGS_SCHEMA_VERSION);
    nvs_set_blob(my_handle, "schedule", &programSchedule, sizeof(programSchedule));
    nvs_set_i8(my_handle, "tempUnit", (int8_t)tempUnit);
    nvs_set_i8(my_handle, "systemMode", (int8_t)systemMode);
    nvs_set_i8(my_handle, "vacation", (int8_t)vacationModeActive);

    nvs_commit(my_handle);
    nvs_close(my_handle);
}

// Learned performance blob only. Cycle completions just mark performanceDirty;
// the control tick batches actual writes to one per PERF_FLUSH_INTERVAL_MINS.
void savePerformance() {
    if (g_isTesting) return;
    nvs_handle_t my_handle;
    if (nvs_open("hvac_cfg", NVS_READWRITE, &my_handle) != ESP_OK) return;
    nvs_set_blob(my_handle, "perf", &performance, sizeof(performance));
    nvs_commit(my_handle);
    nvs_close(my_handle);
    performanceDirty = false;
}

// Fault/lockout must survive a reboot: a power blip (or a panic mid-lockout)
// must not silently un-latch a safety state. Written immediately on change —
// these fire a handful of times per equipment lifetime, so wear is a non-issue.
void saveSafetyState(const char* reason) {
    if (g_isTesting) return;
    if (reason) snprintf(g_safetyReason, sizeof(g_safetyReason), "%s", reason);
    else g_safetyReason[0] = '\0';
    nvs_handle_t my_handle;
    if (nvs_open("hvac_cfg", NVS_READWRITE, &my_handle) != ESP_OK) return;
    nvs_set_u8(my_handle, "fault", systemInFaultState ? 1 : 0);
    nvs_set_u8(my_handle, "lockout", systemLockedOut ? 1 : 0);
    nvs_set_str(my_handle, "safetyWhy", g_safetyReason);
    nvs_commit(my_handle);
    nvs_close(my_handle);
}

void loadSettings() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("hvac_cfg", NVS_READONLY, &my_handle);
    if (err != ESP_OK) { saveSettings(); return; }

    // Struct layouts change across firmware versions; a schema mismatch means
    // the stored blobs are unreadable — keep compiled defaults and re-save.
    uint8_t storedSchema = 0;
    nvs_get_u8(my_handle, "schema", &storedSchema);
    if (storedSchema != SETTINGS_SCHEMA_VERSION) {
        nvs_close(my_handle);
        Serial.printf("NVS schema %u != firmware %u - discarding stored settings, using defaults.\n",
                      storedSchema, SETTINGS_SCHEMA_VERSION);
        saveSettings();
        return;
    }

    size_t required_size = sizeof(programSchedule);
    nvs_get_blob(my_handle, "schedule", &programSchedule, &required_size);

    size_t perf_size = sizeof(performance);
    nvs_get_blob(my_handle, "perf", &performance, &perf_size);

    int8_t temp_i8;
    if(nvs_get_i8(my_handle, "tempUnit", &temp_i8) == ESP_OK) tempUnit = (TempUnit)temp_i8;
    if(nvs_get_i8(my_handle, "systemMode", &temp_i8) == ESP_OK) systemMode = (SystemMode)temp_i8;
    if(nvs_get_i8(my_handle, "vacation", &temp_i8) == ESP_OK) vacationModeActive = (bool)temp_i8;

    // Restore latched safety state: it clears only via explicit acknowledge
    // (`reset fault` / `reset lockout` on the console or web), never by reboot.
    uint8_t u8 = 0;
    if (nvs_get_u8(my_handle, "fault", &u8) == ESP_OK && u8) systemInFaultState = true;
    u8 = 0;
    if (nvs_get_u8(my_handle, "lockout", &u8) == ESP_OK && u8) systemLockedOut = true;
    size_t reasonSize = sizeof(g_safetyReason);
    if (nvs_get_str(my_handle, "safetyWhy", g_safetyReason, &reasonSize) != ESP_OK) {
        g_safetyReason[0] = '\0';
    }

    nvs_close(my_handle);
}

// =================================================================
// ==                 TIME & SENSOR VALIDATION                    ==
// =================================================================
TimeInfo getCurrentTime() {
  if (g_isTesting) return g_mockTime;
  TimeInfo sim;
  if (console_simTime(sim)) return sim;
  TimeInfo now = {0, 0, 0, 0, 0};
  if (isTimeValid()) {
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    now.month = lt.tm_mon + 1;
    now.day = lt.tm_mday;
    now.dayOfWeek = lt.tm_wday;
    now.hour = lt.tm_hour;
    now.minute = lt.tm_min;
  }
  return now;
}

int getCurrentSeason() {
    TimeInfo now = getCurrentTime();
    int month = now.month;
    if (month == 12 || month == 1 || month == 2) return 0; // Winter
    if (month >= 3 && month <= 5) return 1;               // Spring
    if (month >= 6 && month <= 8) return 2;               // Summer
    return 3;                                             // Fall
}

bool validateSensorReadings(float indoorTempF) {
    // isnan check is required: NaN compares false against everything, so a
    // failed sensor returning NaN would otherwise pass the range check.
    if (isnan(indoorTempF) || indoorTempF < MIN_PLAUSIBLE_TEMP_F || indoorTempF > MAX_PLAUSIBLE_TEMP_F) {
        Serial.printf("CRITICAL FAULT: Indoor temperature reading (%.1f F) is outside plausible range!\n", indoorTempF);
        return false;
    }
    return true;
}

// =================================================================
// ==                     CONTROL TICK                            ==
// =================================================================
static void controlTick() {
  static int tickCount = 0;
  tickCount++;

  if (systemInFaultState) {
    writeRelay(HEATER_RELAY, LOW); writeRelay(COOLER_RELAY, LOW);
    writeRelay(FAN_RELAY, LOW);    writeRelay(FRESH_AIR_RELAY, LOW);
    writeRelay(HUMIDITY_RELAY, LOW);
    if (tickCount % 6 == 1) {
      Serial.println("System in FAULT state - all relays off. Fix the cause, then `reset fault` on the console.");
    }
    return; // console stays live; no blocking delay needed in a non-blocking loop
  }

  // Service mode: a human owns the relays; normal control stays out of the way.
  if (console_testModeActive()) return;

  float currentTempRaw = readTemperature();
  float outdoorTempRaw = readOutdoorTemperature();
  float currentHumidity = readHumidity();

  float indoorF = (tempUnit == FAHRENHEIT) ? currentTempRaw : celsiusToFahrenheit(currentTempRaw);

  // Not commissioned yet (no probe has ever reported): wait quietly instead of
  // latching a fault. Faults are for sensors that existed and then died.
  if (isnan(indoorF) && !sensors_hasEverHadData()) {
    if (tickCount % 6 == 1) {
      Serial.println("Waiting for first indoor reading (remote probe report, SHT4x, or console 'remote'/'sim temp').");
    }
    return;
  }

  if (!validateSensorReadings(indoorF)) {
    systemInFaultState = true;
    logEvent("FAULT: indoor sensor implausible (%.1fF)", indoorF);
    saveSafetyState("indoor sensor implausible");
    return;
  }
  lastIndoorF = indoorF;
  health_poll();

  // The outdoor sensor is non-critical: on an implausible reading, substitute
  // the indoor temperature, which neutralizes both economizer comparisons
  // without blocking heating/cooling.
  float outdoorCheckF = (tempUnit == FAHRENHEIT) ? outdoorTempRaw : celsiusToFahrenheit(outdoorTempRaw);
  bool outdoorValid = !(isnan(outdoorCheckF) || outdoorCheckF < MIN_PLAUSIBLE_OUTDOOR_TEMP_F || outdoorCheckF > MAX_PLAUSIBLE_OUTDOOR_TEMP_F);
  if (!outdoorValid) outdoorTempRaw = currentTempRaw;

  bool humidityValid = !isnan(currentHumidity) && currentHumidity >= 0.0 && currentHumidity <= 100.0;

  analyze_and_learn_if_needed();

  // Batched learning-data persistence (flash wear: ~4 writes/day, not ~75).
  static unsigned long lastPerfFlushMs = 0;
  if (performanceDirty && millis() - lastPerfFlushMs > PERF_FLUSH_INTERVAL_MINS * 60000UL) {
    lastPerfFlushMs = millis();
    savePerformance();
    health_flushIfDirty();
    logEvent("learning + health data flushed to NVS");
  }


  ResolvedTargets targets = resolveTargets(indoorF, outdoorValid ? outdoorCheckF : NAN);

  // Overcool-to-dehumidify: the A/C is the only dehumidifier; when RH runs
  // high, allow cooling slightly past the cool setpoint (with hysteresis).
  static bool overcoolActive = false;
  if (ENABLE_OVERCOOL_DEHUMIDIFY && humidityValid &&
      (targets.effectiveMode == SYS_COOL || targets.effectiveMode == SYS_AUTO)) {
    if (currentHumidity > DEHUMIDIFY_RH_THRESHOLD) {
      if (!overcoolActive) logEvent("overcool dehumidify ON (%.0f%%RH)", currentHumidity);
      overcoolActive = true;
    } else if (currentHumidity < DEHUMIDIFY_RH_THRESHOLD - HUMIDITY_DEADBAND) {
      if (overcoolActive) logEvent("overcool dehumidify off");
      overcoolActive = false;
    }
  } else {
    overcoolActive = false;
  }
  if (overcoolActive) targets.coolSetpointF -= DEHUMIDIFY_OVERCOOL_MAX_F;

  if (targets.source != lastResolved.source) {
    logEvent("targets: %.1f/%.1fF [%s]", targets.heatSetpointF, targets.coolSetpointF, targetSourceName(targets.source));
  }
  lastResolved = targets;

  float tempC = (tempUnit == FAHRENHEIT) ? fahrenheitToCelsius(currentTempRaw) : currentTempRaw;
  float outdoorTempC = (tempUnit == FAHRENHEIT) ? fahrenheitToCelsius(outdoorTempRaw) : outdoorTempRaw;
  // Setpoints are always stored in F.
  float heatTargetC = fahrenheitToCelsius(targets.heatSetpointF);
  float coolTargetC = fahrenheitToCelsius(targets.coolSetpointF);

  controlTemperature(tempC, heatTargetC, coolTargetC, outdoorTempC, targets.effectiveMode);
  // Fan before humidity: the humidifier's airflow gate reads this tick's fan
  // decision, not last tick's.
  controlFan(targets.fanMode);
  if (humidityValid) {
    controlHumidity(currentHumidity, tempC, outdoorTempC);
  } else if (readRelay(HUMIDITY_RELAY)) {
    writeRelay(HUMIDITY_RELAY, LOW); // don't humidify on a failed sensor
  }

  // Telemetry line every ~30s (every tick would drown the console).
  if (tickCount % 6 == 1) {
    TimeInfo now = getCurrentTime();
    Serial.printf("%02d:%02d | %.1fF (heat-to %.1f / cool-to %.1f, %s) | H:%d C:%d F:%d D:%d Hum:%d\n",
      now.hour, now.minute, indoorF, targets.heatSetpointF, targets.coolSetpointF,
      targetSourceName(targets.source),
      readRelay(HEATER_RELAY), readRelay(COOLER_RELAY), readRelay(FAN_RELAY),
      readRelay(FRESH_AIR_RELAY), readRelay(HUMIDITY_RELAY));
  }
}

// =================================================================
// ==                     SETUP & LOOP                            ==
// =================================================================
void setup() {
  Serial.begin(115200);
  // Deployed, nothing is draining the USB CDC. Without a zero TX timeout the
  // HWCDC ring fills and every Serial write blocks the control loop — with
  // relays energized, and MOD-IO2 contacts that stay closed until we rewrite
  // the mask. Logging must never be able to stall control.
  Serial.setTxTimeoutMs(0);
  // On the C6 the console is USB-Serial-JTAG: blocking on !Serial forever would
  // hang a headless boot, so wait at most 3 s for a monitor to attach.
  for (unsigned long t0 = millis(); !Serial && millis() - t0 < 3000; ) delay(10);

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  loadSettings();
  // Filesystem init must come before the watchdog is armed: a first-boot
  // format can block far longer than the WDT timeout.
  initialize_learning();
  events_begin(); // flash journal (needs LittleFS); earlier events stay RAM-only

  if (systemInFaultState || systemLockedOut) {
    // Terse: the RAM ring caps messages at 56 chars and the reason must fit.
    logEvent("SAFETY restored: %s%s(%s)",
             systemInFaultState ? "FAULT " : "", systemLockedOut ? "LOCKOUT " : "",
             g_safetyReason[0] ? g_safetyReason : "reason unknown");
    Serial.printf("SAFETY STATE RESTORED: %s%s(%s). Fix the cause, then 'reset fault' / 'reset lockout'.\n",
                  systemInFaultState ? "FAULT " : "", systemLockedOut ? "LOCKOUT " : "",
                  g_safetyReason[0] ? g_safetyReason : "reason unknown");
  }

  // IDF 5 / Arduino core 3.x: the task WDT is already initialized at startup,
  // so reconfigure it (fall back to init if this build has it disabled).
  esp_task_wdt_config_t twdt_config = {};
  twdt_config.timeout_ms = WDT_TIMEOUT_SECS * 1000;
  twdt_config.idle_core_mask = 0;
  twdt_config.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&twdt_config) != ESP_OK) {
      esp_task_wdt_init(&twdt_config);
  }
  esp_task_wdt_add(NULL);

  runTests();

  initRelays(); // EVB GPIO relays + MOD-IO2 I2C bus, everything driven off
  statusLed_begin();
  inputs_begin(); // before health_begin: it restores the commissioned mask
  sensors_begin();
  net_time_begin();
  mqtt_begin();
  console_begin();
  health_begin();
  initialize_logic_timers();
  logEvent("boot complete (reset reason: %s)", resetReasonName((int)esp_reset_reason()));

  Serial.println("Initialization Complete. Applying power-on delay...");
  safeDelay(5000);
  Serial.println("Starting control loop. Type 'help' for console commands.");
}

void loop() {
  esp_task_wdt_reset();

  console_poll();   // serial commands, sim clock, service-mode safety timers
  statusLed_poll(); // blink codes (fault/lockout/service/health/heartbeat)
  inputs_poll();    // opto sampling (fast: must catch 60Hz half-wave pulses)
  net_time_poll();  // WiFi reconnect state machine
  weather_poll();   // outdoor temp fetch (no-op without WiFi/coordinates)
  mqtt_poll();      // HA entity + Zigbee2MQTT probe ingestion (no-op unconfigured)
  ota_poll();       // OTA updates (no-op without password)
  web_poll();       // local dashboard (starts once WiFi is up)

  static unsigned long lastControlMs = 0;
  unsigned long nowMs = millis();
  if (nowMs - lastControlMs >= CONTROL_INTERVAL_MS) {
    lastControlMs = nowMs;
    controlTick();
  }

  delay(10); // yield; keeps the loop responsive for console input
}
