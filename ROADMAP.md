# Roadmap

Goal: the best single-zone smart thermostat we can build on the Olimex
ESP32-C6-EVB + MOD-IO2 stack — locally controlled, safe by construction,
serviceable by an HVAC tech, and genuinely smarter than commercial stats.

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

## Phase 6 — Differentiators

- [ ] CO₂ sensor (SCD40 on UEXT) → demand-controlled ventilation with the fresh-air damper
- [ ] **Aqara probe path B**: C6 as native Zigbee coordinator — no hub needed. Arduino core 3.x ships a Zigbee library for the C6's 802.15.4 radio; needs Zigbee build flags/partitions and WiFi-coexistence validation under pioarduino, plus ZCL temperature-cluster parsing. Verify feasibility before committing
- [ ] Multi-room: several probes with averaging/weighting (and occupancy weighting later)
- [ ] Equipment profiles: 2-stage heat (spare relay as W2), heat pump w/ O/B + aux lockout temps
- [ ] Matter support (stretch)
- [ ] Occupancy learning from the manual-adjustment log

## Standing invariants

- All builds and tests run in Docker (`scripts/docker-build.sh`).
- Heat/cool/fan/damper stay on EVB GPIO relays; the I²C expander only ever carries loads that are harmless when stranded.
- Every wait longer than the WDT timeout goes through `safeDelay()`.
- Logic code touches hardware only through the HAL (`relays.cpp`, sensor/time wrappers).
- Boot self-tests must restore all state they mutate and never write NVS.
