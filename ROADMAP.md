# Roadmap

Goal: the best single-zone smart thermostat we can build on the Olimex
ESP32-C6-EVB + MOD-IO2 stack — locally controlled, safe by construction,
serviceable by an HVAC tech, and genuinely smarter than commercial stats.

## Architecture decision (2026-10-28)

Required: a WiFi-hosted site/app, Zigbee temperature sensors, and Matter.
Chosen: **keep the single C6 and get Zigbee and Matter through Home Assistant**.

- **Web UI/API**: served by the thermostat itself over WiFi (built, Phase 4).
- **Zigbee sensors**: Aqara path A, i.e. probes join Zigbee2MQTT and reach the
  thermostat over MQTT (built). This also keeps the sensors usable by HA and
  everything else, because a Zigbee device can join only one network, and a
  native coordinator on the thermostat would own them.
- **Matter**: no Matter stack in the firmware. Matterbridge (HA add-on with
  its Home Assistant plugin) re-exposes the MQTT climate entity as a Matter
  thermostat to Apple Home / Google / Alexa. HA's built-in Matter integration
  is a controller only and does not export entities. Firmware cost: none.
- **Consequence**: indoor sensing now depends on HA/Z2M/broker/WiFi staying up,
  so surviving a hub outage is a Phase 5 requirement (below).
- **Rejected for now**: native Zigbee + Matter on one 4 MB C6 (app + ZBOSS +
  Matter ≈ 2.5–3 MB per copy, plus unproven 3-way radio sharing). If the
  thermostat later has to work without a hub, the path is a second chip
  (H2/C6) as a dedicated Zigbee coordinator over UEXT UART, which is Espressif's
  reference gateway split.

## Phase 1 — Foundation (done)

- [x] NTP time + timezone (WiFi via gitignored `secrets.h`; safe fallback hold when time is unknown)
- [x] SHT4x driver on UEXT I²C (temperature + humidity, CRC-checked) — now an *optional local fallback*; primary sensing is remote wireless probes
- [x] Remote-sensor ingestion API with staleness timeout (`sensors_injectRemote`, transport-agnostic — Zigbee/MQTT/ESP-NOW plug in later); "never commissioned" waits quietly, "sensor died" faults
- [x] Outdoor sensor honesty: return NAN until real hardware exists (economizer stays disabled)
- [x] Dual heat/cool setpoints per schedule entry, enforced minimum gap; vacation becomes a 55/85 pair
- [x] Setpoint resolution pipeline with provenance (schedule → carryover → vacation → hold → protection clamps)
- [x] Freeze protection (force heat < 45 °F) and high-temp protection (force cool > 92 °F) in every mode
- [x] Non-blocking control loop (millis scheduler; console stays live even in fault state)
- [x] NVS schema versioning (stale blobs discarded instead of silently mis-loaded)

## Phase 2 — Debug console & simulation (done)

- [x] Serial command console on USB-C: status w/ target provenance, sensors, relay control, mode/hold, fault/lockout reset, i2c scan, event log
- [x] Runtime simulation: override sensors/clock, time-warp speed multiplier, NaN/failure injection
- [x] Service (test) mode with guardrails: control suspended, per-relay auto-off timer, idle timeout, compressor min-off honored unless per-action override, nothing persists across reboot
- [x] Event ring buffer (relay transitions, faults, mode changes) feeding `log`

## Phase 3 — Control polish (done)

- [x] Smart recovery: consume the learned seasonal rates to start heat/cool early and hit schedule times (default 3°F/hr until data accumulates)
- [x] Fan-off delay after heat cycles (90 s purge; deliberately no cool-side purge)
- [x] Humidifier gated on airflow and never during cooling; overcool-to-dehumidify clamp (max 1.5 °F below cool setpoint, hysteresis on RH threshold)
- [x] Circulate rework: 15 fan-minutes/hour quota, deadline-scheduled, heat/cool/purge runtime credited
- [x] Weather API (open-meteo, plain HTTP) as outdoor-temp source with 30-min staleness (coordinates in secrets.h)
- [ ] Forecast-aware pre-conditioning (hourly forecast + pre-cool/pre-heat planning) — moved to Phase 4+

## Phase 4 — Connectivity (core done)

- [x] Custom partition table: dual 1.5MB OTA slots on the EVB's REAL 4MB flash (WROOM-1-N4 — the devkitc-1 board def wrongly claims 8MB), 32KB NVS, 832KB LittleFS, coredump partition
- [x] Local web dashboard on port 80: live status w/ provenance + health, mode buttons, hold form, fault/lockout reset, /api/* JSON endpoints
- [x] MQTT climate entity with Home Assistant discovery (dual setpoints in °F, action/availability/LWT, outdoor + health sensors; HA setpoint changes map to temporary holds)
- [x] **Aqara probe path A**: Zigbee2MQTT topics (secrets.h) → `sensors_injectRemote()` (indoor, w/ humidity) and `sensors_injectOutdoorProbe()` (outdoor); low-battery reports logged as health events
- [x] OTA updates via ArduinoOTA (password-gated; relays off + data flushed + WDT released during flash)
- [ ] Web schedule editor + AP-mode WiFi provisioning (secrets.h workflow covers until then)
- [ ] Tech test wizard on web + downloadable service report (console service mode covers until then)

## Phase 5 — Reliability depth

- [x] S.M.A.R.T.-style health rules (advisory): no-response calls (temp trend), capacity degradation vs learned baseline, short-cycling, filter runtime, MOD-IO2 bus failures, abnormal resets, sensor dropouts — persisted counters, console `health` report
- [x] Opto-input equipment feedback (24VAC sense on OPT1–3: heat/cool/fan call-proving + phantom-run detection, self-arming on first signal; OPT4 window contact logged) — strengthens the no-response rule from temp-trend inference to direct proof; field wiring still pending at install
- [x] Persistent lockout/fault across reboots with explicit acknowledge (NVS-latched with trip reason; cleared only by `reset fault`/`reset lockout` on console or web)
- [x] Event journal persisted to LittleFS (rotating, rate-limited; console `log file`); reset-reason logging on every boot
- [ ] Host-native PlatformIO test env + CI (GitHub Actions running the Docker build + logic tests)
- [x] LED blink-codes for fault classes (EVB user LED GPIO8: fault/lockout/service/health/heartbeat patterns)
- [ ] **Hub-outage resilience**: today a remote probe going stale (>90 min) with no SHT4x fitted reads NAN, which latches a persistent FAULT with all relays (heat included) off. That fault survives the hub's recovery and needs a manual `reset fault`. Stale remote data should become a non-latching degraded state that clears on the next report and keeps freeze protection alive. Hardware backstop: fit the SHT4x so the existing local fallback takes over.

## Phase 6 — Differentiators

- [ ] CO₂ sensor (SCD40 on UEXT) → demand-controlled ventilation with the fresh-air damper
- [ ] **Aqara probe path B**: C6 as native Zigbee coordinator — no hub needed. **Feasibility spike run 2026-08-14** (`zigbee_spike` env, kept in-tree for re-runs): toolchain fully works under pioarduino (`-DZIGBEE_MODE_ZCZR` + zb partitions; coordinator forms a network on-device; `ZigbeeThermostat` endpoint receives temp+RH — no hand-rolled ZCL parsing needed). **Cost finding**: the ZBOSS ZCZR stack adds ~388 KB flash / ~19 KB RAM → full firmware would be ~1.6 MB, so on the 4 MB module **Zigbee coordinator and dual-slot OTA are mutually exclusive** (single ~3 MB app fits easily; any OTA does not). **Unresolved**: RF-level WiFi coexistence quality — now measurable: the bench board's WiFi is healthy. (A long 2026-08-14 debug chased apparently-dead WiFi RX — empty scans, STA NO_AP_FOUND, failed client associations, per-boot deaf/alive lottery — through frameworks, calibration, beacon formats, channels, and silicon theories. **Root cause: an inadequate USB power supply/cable.** On a good supply the same board hears ~700 beacons/10 s across six SSIDs, every boot. Lesson, now standing policy: for flaky embedded RF, check power delivery FIRST. The `zigbee_spike_baseline` env's beacon-rate sniffer is the 2-minute RF health check: healthy = hundreds of beacons/10 s.) Espressif rates WiFi-STA + router-role 802.15.4 coex "conditionally stable" and recommends dual-SoC for gateways; community reports are negative. **Verdict: stay on path A (Zigbee2MQTT) as primary; revisit path B only if dropping OTA (or moving to an 8 MB module / second C6 as a dedicated coordinator) is acceptable — after the antenna question is resolved.** *Correction (2026-10-28): "mutually exclusive" holds only for the current table. LittleFS is 832 KB but the firmware caps its use at ~250 KB (64 KB learning log, 2×48 KB journal), so a re-partition (256 KB LittleFS + zb areas) leaves ~1.77 MB per OTA slot vs the ~1.6 MB estimate. That estimate is spike delta + current build, not a measured full build. Radio coexistence stays the real blocker. Moot under the 2026-10-28 architecture decision.*
- [ ] Multi-room: several probes with averaging/weighting (and occupancy weighting later)
- [ ] Equipment profiles: 2-stage heat (spare relay as W2), heat pump w/ O/B + aux lockout temps
- [ ] Matter: via Matterbridge on Home Assistant, no firmware stack (see the architecture decision). Verify that the MQTT climate entity's dual setpoints, mode and action map cleanly to the Matter thermostat cluster
- [ ] Occupancy learning from the manual-adjustment log

## Standing invariants

- All builds and tests run in Docker (`scripts/docker-build.sh`).
- Heat/cool/fan/damper stay on EVB GPIO relays; the I²C expander only ever carries loads that are harmless when stranded.
- Every wait longer than the WDT timeout goes through `safeDelay()`.
- Logic code touches hardware only through the HAL (`relays.cpp`, sensor/time wrappers).
- Boot self-tests must restore all state they mutate and never write NVS.
