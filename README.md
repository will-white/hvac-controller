# ESP32-C6 Smart HVAC Thermostat

This project is a feature-rich, single-zone HVAC thermostat built on the ESP32-C6 microcontroller. It is designed to be a robust and intelligent replacement for standard programmable thermostats, incorporating advanced safety, efficiency, and learning features typically found in high-end commercial products. All logic is processed locally on the device, ensuring privacy and offline functionality.

## Key Features

### Core Control
* **7-Day Programmable Schedule**: Independent schedules for weekdays and weekends with up to 10 custom periods per day (e.g., Wake, Away, Home, Sleep). Every period carries a **heat-to / cool-to setpoint pair** with an enforced minimum gap, so auto-changeover can never make the stages fight; before the day's first period, the previous night's period carries over.
* **Layered Target Resolution**: The active setpoints are resolved through explicit layers — schedule → temporary hold → vacation → protection clamps — and the system always knows *why* the current target is what it is (shown in the console `status`).
* **Multi-Mode Operation**: Supports Heating, Cooling, Auto-Changeover, and Fan-Only modes.
* **Fan Control**: `AUTO` (runs with system, plus a 90 s post-heat purge to harvest exchanger warmth), `ON` (always on), and `CIRCULATE` (guarantees a fan-minutes-per-hour quota, with heat/cool runtime credited toward it — the fan only adds what the equipment didn't already provide).
* **Unit Selection**: Supports both Fahrenheit and Celsius for all temperature settings and readouts.

### Efficiency & Smart Features
* **Economizer (Free Cooling/Heating)**: Automatically uses the fresh air intake instead of the furnace or A/C when outdoor temperature is favorable, significantly reducing energy consumption. Outdoor temperature comes from the open-meteo weather API (set coordinates in `secrets.h`); with no fresh outdoor data the economizer stays safely disabled.
* **Humidity Done Right**: Humidifier runs only with airflow and never while cooling; a window-condensation limit (Magnus dew-point model) caps the target; and when indoor RH runs high in cooling season, the system *overcools slightly* (configurable, default max 1.5 °F) to wring moisture out with the A/C.
* **Time to Temperature (Smart Recovery)**: Learns the home's heating/cooling rate per season and outdoor-temperature bin, and starts heating or cooling *before* a scheduled change so the target is reached *at* the scheduled time (conservative default rate until data accumulates; shows as `smart recovery` in `status`).
* **Seasonal Performance Learning**: The smart recovery feature learns and stores separate performance data for all four seasons, allowing it to make more accurate predictions as outdoor conditions change throughout the year.
* **Local Learning Framework**: Manual holds are logged to the onboard LittleFS filesystem (size-capped, self-rotating), enabling future algorithms that adjust the schedule based on user behavior.

### Safety & Reliability
* **S.M.A.R.T. Equipment Health**: Drive-style attribute monitoring with thresholds — call-no-response detection (relay on but temperature moving the wrong way), capacity degradation against the learned per-season baseline (dirty filter / weak igniter / low charge), short-cycle detection, filter-runtime service reminder, I²C bus failures, abnormal-reset and sensor-dropout counters. All advisory (never interrupts heating/cooling), persisted through reboots, reported by the console `health` command.
* **Freeze & Heat Protection**: In *every* mode (including OFF and vacation), the system forces heating below 45 °F indoor and cooling above 92 °F — pipe-freeze and extreme-heat protection outrank all user settings.
* **Cycle Protection**: Enforces configurable minimum run times and minimum off times for both heating and cooling equipment to prevent short-cycling and protect the HVAC compressor and components. The compressor min-off period is also enforced across power cycles.
* **Maximum Run Time Protection**: A critical safety feature that shuts down a heating or cooling cycle if it runs for an excessively long time (e.g., 90 minutes), preventing runaway operation due to a fault or open window.
* **System Lockout**: If the maximum run time is triggered multiple times within a set period, the thermostat will enter a full system lockout to protect the HVAC from major damage, requiring a manual reset.
* **Sensor Fault Detection**: Validates that sensor readings are within a plausible range. If a fault is detected, the system enters a safe fault state, shutting down all relays.
* **Hardware Watchdog Timer**: Utilizes the ESP32's built-in watchdog to automatically reboot the device if the main code ever freezes, ensuring the system never gets stuck with a relay on.
* **Power-On Delay**: Waits for a few seconds after booting before starting any HVAC operations to protect against power surges.
* **Persistent Storage, Flash-Wear Aware**: Settings and the schedule are saved to NVS on change; learned performance data is batched (dirty-flag, flushed at most every 6 h, plus on console `save`/`reboot`) so learning never hammers the flash — worst case a handful of NVS writes per day instead of one per equipment cycle.

## Connectivity

With WiFi configured, the controller exposes (each independently opt-in via `secrets.h`):

* **Web dashboard** at the device's IP: live temperature/targets with provenance, relay states, mode buttons, temporary holds, and fault/lockout reset. JSON API under `/api/` for scripting.
* **Home Assistant** via MQTT auto-discovery: a full climate entity (dual °F setpoints, action state, availability) plus outdoor-temperature and health-warning sensors. Setpoint changes from HA become temporary holds, so the schedule always wins back control.
* **Zigbee2MQTT sensor ingestion**: point `Z2M_INDOOR_TOPIC` / `Z2M_OUTDOOR_TOPIC` at your Aqara sensors' topics and their reports feed the thermostat directly (Celsius payloads converted, humidity included, low-battery logged as a health event).
* **OTA updates** (`pio run -t upload --upload-port hvac-controller.local` with `upload_protocol = espota`): password-gated, drops all relays and flushes learned data before flashing, dual app slots via the custom 4 MB partition table.

## Debug & Service Console

A command console runs on the USB-C serial port (115200 baud) — for development *and* for HVAC technicians. Type `help` for the full list:

| Command | Purpose |
|---|---|
| `status`, `sensors`, `relays`, `log`, `i2c` | Live state: targets + *why* (provenance), sensor readings, relay states, event history, I²C bus scan |
| `health`, `health reset filter` | S.M.A.R.T.-style equipment report: attribute / value / threshold / status table; filter counter reset after a change |
| `remote <tempF> [rh]`, `outdoor <tempF>` | Emulate indoor/outdoor wireless probe reports (exercise the real ingestion + staleness + priority paths) |
| `mode`, `hold <heat> <cool> [h]`, `vacation` | Runtime control: operating mode, temporary hold, vacation toggle |
| `test on` → `relay <name> on\|off [force]` | **Service mode**: suspends normal control so a tech can exercise each relay directly. Guardrails: compressor min-off enforced unless `force`d, 5-minute per-relay auto-off, 30-minute idle auto-exit, nothing survives a reboot |
| `sim temp\|outdoor\|rh\|time\|speed` | **Simulation**: override any sensor (including `nan` for failure injection) or the clock, and time-warp the whole control loop (`sim speed 60` = cycle timers run 60×) |
| `reset fault`, `reset lockout` | The manual reset paths for the safety states |
| `save`, `reboot`, `factory confirm` | Persistence and lifecycle |

## Hardware

The controller is built from two Olimex boards:

| Board | Role |
|---|---|
| **ESP32-C6-EVB** | Main board: ESP32-C6-WROOM-1-N4 (4 MB flash), 4 onboard relays (10A/240VAC), 4 opto-isolated inputs (≤30VDC), 2 UEXT connectors |
| **MOD-IO2 Rev.B** | UEXT expansion: 2 additional relays (15A/250VAC) driven over I²C by its onboard PIC16F1503 (firmware 3.xx/4.3 command set) |

### Relay map (6 total)

| Logical relay | Physical | HVAC signal |
|---|---|---|
| `HEATER_RELAY` | EVB REL1, GPIO10 | W (heat call) |
| `COOLER_RELAY` | EVB REL2, GPIO11 | Y (cool call) |
| `FAN_RELAY` | EVB REL3, GPIO22 | G (blower) |
| `FRESH_AIR_RELAY` | EVB REL4, GPIO23 | Fresh-air damper |
| `HUMIDITY_RELAY` | MOD-IO2 relay 1 | Humidifier |
| `SPARE_RELAY` | MOD-IO2 relay 2 | Unassigned |

The four 24VAC signals critical to heating/cooling are deliberately on the EVB's native GPIO relays; only the humidifier (harmless if stranded) depends on the I²C expander.

### MOD-IO2 I²C interface

Connected via UEXT (**SDA = GPIO6, SCL = GPIO7**), default address **0x21**. Relays are set with command `0x40` + a bitmask byte (`bit0` = relay 1, `bit1` = relay 2). Relay-state readback (`0x43`) only exists from firmware 3.02, so the driver (`src/relays.cpp`) is write-through with a cached state mask. The address is changeable via command `0xF0` with the PGM1 jumper closed (useful if stacking more MOD-IO2s).

### Sensing

Primary indoor sensing is **remote wireless probes** (Zigbee Aqara temperature/humidity sensors, per the roadmap: first via a Zigbee2MQTT bridge, later possibly with the C6 as native Zigbee coordinator on its 802.15.4 radio). The firmware side is already in place: `sensors_injectRemote()` accepts reports from any transport and applies a staleness window (90 min, covering Aqara's hourly heartbeat), and the console `remote` command emulates a report for bench work. An SHT4x on the UEXT I²C bus is supported as an *optional* wired local fallback. An **outdoor** Zigbee probe is supported the same way (`sensors_injectOutdoorProbe()`, console `outdoor`): while its report is fresh (90 min) it outranks the weather API, which serves as fallback. Until any indoor probe has reported, the controller waits quietly with relays off rather than faulting.

### Other EVB I/O (available, not yet used)

Opto-isolated inputs OPT1–4 on GPIO 1/2/3/15 (candidates: window switches, equipment-status sense), user LED on GPIO8, user/boot button on GPIO9, UART0 on GPIO16/17 (ESP-PROG header).

### Power

- **ESP32-C6-EVB**: USB-C (development) or 8–50V DC on its barrel jack (~0.06 A @ 12V).
- **MOD-IO2**: logic runs from UEXT 3.3V, but **the relays only actuate with 12V DC on the MOD-IO2's own barrel jack**.
- A single 12V supply can feed both barrel jacks.

> **The two relay banks fail differently.** The EVB's REL1–REL4 are GPIO-driven and fail *open* — any reset releases them. The MOD-IO2's relays do not: the expander latches its last commanded mask on its own 12V rail, so if the ESP32 stops for any reason its contacts stay **closed** until either the firmware runs again or the EVB loses power (which drops UEXT 3.3V and the expander with it). This is why heating, cooling, fan and damper are wired to the EVB relays, and why any sketch that drives the expander keeps the task watchdog armed.

## Software Architecture

The project is organized into a clean, modular, multi-file structure to separate concerns and improve maintainability.

### `main.cpp` / `main.h`
* **Role**: The main application entry point and coordinator.
* **Responsibilities**:
  * Initializes all hardware and software modules (`Serial`, NVS, Watchdog, Filesystem).
  * Loads settings from NVS on boot.
  * Runs the main `loop()`, which orchestrates calls to all other modules.
  * Handles top-level logic like sensor fault detection and smart recovery calculations.
  * Contains the time/sensor abstraction layer (e.g., `currentTime`, `readTemperature`) to facilitate testing.

### `relays.cpp` / `relays.h`
* **Role**: Relay hardware abstraction.
* **Responsibilities**:
  * Maps the logical relay indices from `config.h` to physical channels: EVB GPIO relays or MOD-IO2 I²C relays.
  * Implements the MOD-IO2 driver (command `0x40`, cached state mask, presence detection with a warning if the board doesn't answer).
  * Honors the `g_isTesting` mock layer so tests exercise relay logic without hardware.

### `config.h`
* **Role**: A centralized header for all system-wide configurations.
* **Responsibilities**:
  * Defines all user-configurable settings (default temperature unit, cycle times, etc.).
  * Contains all hardware pin assignments and data structures like `Schedule` and `HvacPerformance`.
  * Holds all `enum` definitions for modes like `SystemMode` and `FanMode`.

### `hvac_logic.cpp` / `hvac_logic.h`
* **Role**: The core HVAC control engine.
* **Responsibilities**:
  * Contains the primary `controlTemperature`, `controlFan`, and `controlHumidity` functions.
  * Implements all the rules-based logic for when to turn relays on or off.
  * Manages all cycle protection timers and the safety lockout logic.
  * Contains the logic for measuring and learning HVAC performance data after each cycle.

### `learning.cpp` / `learning.h`
* **Role**: The module for on-device machine learning.
* **Responsibilities**:
  * Initializes the SPIFFS/LittleFS filesystem.
  * Provides the `log_manual_adjustment()` function to write user overrides to a log file.
  * Contains the placeholder `analyze_and_learn_if_needed()` function.

### `hvac_tests.cpp` / `hvac_tests.h`
* **Role**: A self-contained suite for unit and logic testing.
* **Responsibilities**:
  * Contains the `runTests()` function, which is called from `setup()` on boot.
  * Includes specific test functions for every major feature.
  * Uses a mocking framework to simulate time, sensor inputs, and relay states.

## Setup & Installation
1. **Build (Docker, recommended)**: All building happens in a Docker container — no host toolchain needed. Run `scripts/docker-build.sh` to build the firmware, `scripts/docker-build.sh -e test_relays` for the relay hardware test, or `scripts/docker-build.sh -e blank` for the all-relays-off sketch. VS Code users can instead open the repo in the provided devcontainer (`.devcontainer/`), which uses the same image and package cache. **On Windows use `scripts/build.ps1` instead** — the shell script fails from Git Bash, which rewrites the container-side working directory.
2. **Target**: ESP32-C6 (`esp32-c6-devkitc-1`) via the [pioarduino](https://github.com/pioarduino/platform-espressif32) espressif32 platform (Arduino core 3.x). The official PlatformIO espressif32 platform has no C6 support.
3. **File Structure**: All sources and headers live together in `src/`.
4. **Libraries**: One external library — `knolleary/PubSubClient` for MQTT, declared in `platformio.ini` and fetched automatically. Everything else ships with the Arduino core.
5. **Configuration**: Modify the settings in `src/config.h` to match your HVAC system (including the `TZ_INFO` timezone string).
6. **WiFi (optional — needed only at deployment)**: copy `src/secrets.h.example` to `src/secrets.h` and fill in credentials for a real NTP clock. Without it the firmware runs offline: the schedule is replaced by a safe fallback hold (68/76 °F), and on the bench `sim time` provides a clock for schedule testing.
7. **Upload & Monitor**: from the devcontainer, `pio run -e hvac_controller -t upload` with the board's USB passed through (WSL: `usbipd attach`). On Windows, `scripts/flash.ps1` builds in Docker and then flashes from the host with esptool — no host toolchain needed. Monitor at `115200` baud (`scripts/monitor.ps1`) to view the test-suite results and status logs.

   > **Do not let a serial monitor assert RTS.** On the C6's native USB-Serial-JTAG, DTR/RTS drive RESET and GPIO9/boot, so asserting them knocks the *running* firmware into the ROM download bootloader — you'll see an `ESP-ROM:` banner and nothing more. The EVB relays open on that reset, but any MOD-IO2 relay stays latched closed until the firmware runs again. `platformio.ini` pins `monitor_dtr`/`monitor_rts` to `0`; output still flows, because CDC connection is detected from USB SOF rather than DTR.