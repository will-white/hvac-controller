# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A single-zone HVAC thermostat on Olimex hardware: an **ESP32-C6-EVB** main board (4 onboard relays on GPIO 10/11/22/23, 4 opto-inputs on GPIO 1/2/3/15) plus a **MOD-IO2 Rev.B** UEXT expansion (2 more relays over I²C, address 0x21, command 0x40+bitmask; needs its own 12V on its barrel jack to actuate). Built with Arduino core 3.x via the pioarduino PlatformIO platform. All control logic runs locally: 7-day scheduling, heat/cool/auto/fan modes, economizer (free heating/cooling via a fresh-air damper), per-season performance learning, and layered safety protections (cycle timers, max-run lockout, sensor fault detection, task watchdog). Full hardware details are in README.md's Hardware section.

## Build Commands

**All building happens in Docker — never install PlatformIO or toolchains on the host.**

- `scripts/docker-build.sh` — build the main firmware (default env `hvac_controller`)
- `scripts/docker-build.sh -e test_relays` — build the standalone relay hardware test
- `scripts/docker-build.sh -t clean` — clean build artifacts

The script builds the `.devcontainer/` image and runs `pio run` inside it. Platform/toolchain downloads persist in the `platformio-cache` named Docker volume, which the VS Code devcontainer shares. A warm-cache build takes seconds; a cold one downloads ~1 GB first.

Flashing needs USB access the build container doesn't have: upload from the devcontainer with the board passed through (WSL: `usbipd attach`), using `pio run -e hvac_controller -t upload`; serial monitor at 115200 baud.

The platform is pinned in `platformio.ini` to a specific pioarduino release (the official `platformio/espressif32` platform has no C6 support — do not switch back to it). The two envs share `src/` via `build_src_filter`; each includes exactly one file with `setup()`/`loop()`. Defining `build_src_filter` *replaces* the default `+<*>`, so the main env's filter must stay `+<*> -<main-relay-test.cpp>`.

There is no host-side test runner. Tests are on-device: `runTests()` (in `src/hvac_tests.cpp`) runs from `setup()` on every boot and prints results to serial. To add a test, add a function to `hvac_tests.cpp` and call it from `runTests()`. Because tests run on every boot against live globals, `runTests()` snapshots and restores all state the tests mutate (`performance`, `programSchedule`, `vacationModeActive`), and `saveSettings()` refuses to write NVS while `g_isTesting` is set — preserve both invariants when adding tests.

## Architecture

All source and headers live flat in `src/` (no `include/` directory).

- **`config.h`** — the only place for tunables: logical relay indices (`HEATER_RELAY` … `SPARE_RELAY` — indices, NOT GPIO numbers), UEXT I²C pins (SDA=6, SCL=7) and MOD-IO2/SHT4x addresses, timezone, protection thresholds, cycle/safety timings, `SETTINGS_SCHEMA_VERSION`, and shared types (`Schedule` with heat/cool setpoint *pairs*, `ResolvedTargets`, enums). Everything else includes it. **Bump `SETTINGS_SCHEMA_VERSION` whenever any NVS-persisted struct changes layout** — stale blobs are then discarded instead of mis-loaded.
- **`setpoints.cpp` / `.h`** — layered target resolution with provenance: vacation → hold → schedule/carryover (or no-clock fallback) → **smart recovery** (pulls the next entry's target forward using learned °F/hr rates per season/outdoor-bin, `DEFAULT_RECOVERY_RATE_F_PER_HR` until samples exist, capped at `MAX_RECOVERY_TIME_MINS`) → minimum-gap enforcement → freeze/heat protection clamps (latched, active in every mode, may override the user's mode via `effectiveMode`). `resolveTargets(indoorF, outdoorF)` — outdoor picks the recovery rate bin, NAN allowed. Setpoints are **always °F** end to end; `main.cpp` converts to °C only at the `controlTemperature` boundary. Owns the temporary hold (RAM only, deliberately unpersisted).
- **`weather.cpp` / `.h`** — outdoor temperature from open-meteo over **plain HTTP** (works; no TLS → no mbedTLS flash cost). Coordinates from `secrets.h` (0/0 = disabled). Parser gotcha: `"temperature_2m"` appears twice in the response — always anchor on `"current":` first. Feeds `sensors_injectOutdoorWeather()` (30-min staleness); a local outdoor probe via `sensors_injectOutdoorProbe()` (90-min staleness) always outranks it while fresh.
- **`console.cpp` / `.h`** — serial REPL (status/sensors/relays/hold/mode/sim/test/log/reset/factory), the runtime **sim layer** (sensor and clock overrides, NaN injection), and **service mode** (suspends control for manual relay exercise). It also owns the *virtual clock*: `currentTime()` reads `console_virtualMillis()`, so `sim speed N` time-warps every cycle timer. Service-mode safety timers intentionally run on REAL `millis()`, never virtual. Guardrails to preserve: relay commands require service mode, compressor min-off refusal without `force`, 5-min relay auto-off, 30-min idle exit, no persistence across reboot.
- **`sensors.cpp` / `.h`** — indoor sensing is **remote-first**: wireless probes (Zigbee Aqara planned) report through the transport-agnostic `sensors_injectRemote(tempF, rh)`, subject to a 90-min staleness window on REAL millis (sim speed must never expire a live probe). Read order: unit-test mock → console sim → fresh remote → local SHT4x (optional fallback; CRC-checked driver, 3-strike NAN) → NAN. "Never commissioned" (no source ever reported) waits quietly in `controlTick`; a source that existed and then went NAN faults. Outdoor priority: fresh local probe → fresh weather feed → NAN (which cleanly disables the economizer).
- **`net_time.cpp` / `.h`** — non-blocking WiFi + SNTP. Credentials via gitignored `src/secrets.h` (template: `secrets.h.example`); absent credentials = offline mode where `isTimeValid()` stays false and the schedule yields to a safe fallback hold. `isTimeValid()` returns true under `g_isTesting`.
- **`mqtt.cpp` / `.h`** — PubSubClient-based (the one external lib). HA discovery (climate + outdoor/health sensors, retained configs, LWT availability), 30 s state publishing, command topics (`cmd/mode`, `cmd/heat_sp`, `cmd/cool_sp` — setpoint commands become temporary holds; enforce the gap when mapping), and Zigbee2MQTT ingestion (topics from secrets.h, **Celsius payloads** parsed with the lightweight `jsonNumber` — no JSON lib; battery <15% logs a health event). Buffer is 1280 B because discovery payloads exceed PubSubClient's 256 B default.
- **`ota.cpp` / `.h`** — ArduinoOTA, gated on `OTA_PASSWORD`. Invariants: onStart turns ALL relays off, flushes perf+health, and unsubscribes the task WDT (flash writes stall the loop); onError re-subscribes it.
- **`web.cpp` / `.h`** — WebServer dashboard (self-contained PROGMEM page, no external assets) + `/api/status|mode|hold|holdoff|reset`. Starts lazily once WiFi is up.
- **`events.cpp` / `.h`** — RAM ring buffer behind `logEvent()` (printf-style); relay transitions auto-log from `relays.cpp`. Console `log` dumps it.
- **`relays.cpp` / `.h`** — relay hardware abstraction. `writeRelay`/`readRelay` take *logical* relay indices; the physical map (EVB GPIO vs MOD-IO2 I²C channel) lives only in `RELAY_MAP` here. The MOD-IO2 driver is write-through with a cached mask (Rev.B firmware may lack the 0x43 readback). Heat/cool/fan/damper must stay on EVB GPIO relays — never move them to the I²C expander, so a bus fault can't strand them.
- **`main.cpp` / `main.h`** — entry point and coordinator: NVS load/save (`hvac_cfg` namespace), watchdog setup, schedule resolution (with previous-day carryover before the day's first entry), sensor validation (including NaN checks; a bad outdoor reading disables the economizer for that cycle rather than faulting), and the 5 s control loop calling `controlTemperature` / `controlHumidity` / `controlFan`. Owns the globals other modules use via `extern` in `main.h` (`performance`, `systemLockedOut`, `tempUnit`, `systemMode`).
- **`hvac_logic.cpp` / `.h`** — the control engine: relay decisions, hysteresis, min/max run-time enforcement, max-run lockout, economizer, post-cycle performance measurement, post-heat fan purge, and circulate-as-quota (fan-minutes/hour, deadline-scheduled, all fan runtime credited). `freshAirPurpose` tracks which demand (heating vs cooling) the damper is serving — the AUTO-mode branch gates depend on it; do not gate branches on the raw damper state (that latches the damper open forever, the bug fixed in 2026-08). `controlFan` must run BEFORE `controlHumidity` in the tick (the humidifier's airflow gate reads this tick's fan decision).
- **`learning.cpp` / `.h`** — logs manual adjustments to LittleFS (`/learning_log.csv`, self-rotating at `LEARNING_LOG_MAX_BYTES`); fed by console holds via `setHold()`. `analyze_and_learn_if_needed()` is a placeholder.
- **`health.cpp` / `.h`** — S.M.A.R.T.-style monitoring. Rules are **advisory only — they must never interrupt heating/cooling**. Hooks: `health_reportCycleStart/Rate/MaxRun` from `hvac_logic`, `health_poll` each control tick (no-response via `getActiveCycleInfo` + `lastIndoorF`, filter hours on real millis, remote-probe dropouts). Counters persist as the `health` NVS blob under the same batched-flush policy (`health_flushIfDirty` rides the 6 h timer); abnormal resets save immediately at boot so reboot loops stay visible. Pure evaluators (`health_evalNoResponse`, `health_evalCapacity`) exist for the boot tests — keep new rules testable the same way.
- **`hvac_tests.cpp` / `.h`** — on-device test suite and the mock layer.
- **Persistence & flash wear** — `saveSettings()` (settings+schedule) runs only on actual changes; learned data sets `performanceDirty` and `savePerformance()` flushes it at most every `PERF_FLUSH_INTERVAL_MINS` (6 h), plus on console `save`/`reboot`. NEVER reintroduce per-cycle NVS writes. The LittleFS learning CSV self-rotates at `LEARNING_LOG_MAX_BYTES`.
- **`utils.h`** — inline F↔C conversions.

### Mocking / hardware abstraction

All hardware and time access goes through wrappers — `writeRelay`/`readRelay` in `relays.cpp`, `currentTime`, `readTemperature`, `readOutdoorTemperature`, `getCurrentTime` in `main.cpp` — which return mock state (declared in `hvac_tests.h`) when `g_isTesting` is set. Never call `digitalWrite`/`millis()` directly in logic code. Exception: `safeDelay()` intentionally uses real `millis()` — it feeds the task watchdog during long waits and must never run under mock time. Any wait that can exceed `WDT_TIMEOUT_SECS` must use `safeDelay()`, not `delay()` (a starved WDT panics and reboots).

### Current gaps

Outdoor temperature has no hardware yet (`readOutdoorTemperature()` returns NAN → economizer disabled; Phase 3 adds a weather API). Smart Recovery (early-start before scheduled changes) is still unimplemented: `ENABLE_SMART_RECOVERY` and `MAX_RECOVERY_TIME_MINS` are unused; the performance-data collection side works. See ROADMAP.md for the full phase plan. Flash: custom `partitions_4mb.csv` (dual 1.5 MB OTA slots) sized for the EVB's REAL 4 MB WROOM-1-N4 module — the `esp32-c6-devkitc-1` board def wrongly claims 8 MB; never size partitions past 4 MB. App is at ~77% of its slot.

### Unit convention

Sensor reads are in the unit set by `DEFAULT_TEMP_UNIT` (Fahrenheit by default). Setpoints (`Schedule`, `ResolvedTargets`, holds, protection thresholds) are **always Fahrenheit**. The control functions in `hvac_logic.cpp` take Celsius — `main.cpp` converts at that boundary only. Performance-learning storage, bins (`getPerformanceBin`), and plausibility checks always operate in Fahrenheit regardless of display unit.

## Other

`Pressure_Sensor.md` documents a separate, unrelated planned project (Raspberry Pi + VERIS pressure sensor → Ubidots); it shares no code with the firmware.
