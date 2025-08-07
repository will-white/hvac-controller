# ESP32-C6 Smart HVAC Thermostat

This project is a feature-rich, single-zone HVAC thermostat built on the ESP32-C6 microcontroller. It is designed to be a robust and intelligent replacement for standard programmable thermostats, incorporating advanced safety, efficiency, and learning features typically found in high-end commercial products. All logic is processed locally on the device, ensuring privacy and offline functionality.

## Key Features

### Core Control
* **7-Day Programmable Schedule**: Independent schedules for weekdays and weekends with up to 10 custom periods per day (e.g., Wake, Away, Home, Sleep).
* **Multi-Mode Operation**: Supports Heating, Cooling, Auto-Changeover, and Fan-Only modes.
* **Fan Control**: Includes `AUTO` (runs with system), `ON` (always on), and `CIRCULATE` (runs periodically for a set time, e.g., 15 minutes every hour).
* **Unit Selection**: Supports both Fahrenheit and Celsius for all temperature settings and readouts.

### Efficiency & Smart Features
* **Economizer (Free Cooling/Heating)**: Automatically uses the fresh air intake instead of the furnace or A/C when outdoor temperature is favorable, significantly reducing energy consumption.
* **Time to Temperature (Smart Recovery)**: Proactively learns the thermal performance of your home and starts heating or cooling *before* a scheduled change to reach the target temperature precisely *at* the scheduled time.
* **Seasonal Performance Learning**: The smart recovery feature learns and stores separate performance data for all four seasons, allowing it to make more accurate predictions as outdoor conditions change throughout the year.
* **Local Learning Framework**: A system for logging manual user adjustments to the onboard filesystem (SPIFFS/LittleFS), enabling future implementation of algorithms that can automatically adjust the schedule based on user behavior.

### Safety & Reliability
* **Cycle Protection**: Enforces configurable minimum run times and minimum off times for both heating and cooling equipment to prevent short-cycling and protect the HVAC compressor and components.
* **Maximum Run Time Protection**: A critical safety feature that shuts down a heating or cooling cycle if it runs for an excessively long time (e.g., 90 minutes), preventing runaway operation due to a fault or open window.
* **System Lockout**: If the maximum run time is triggered multiple times within a set period, the thermostat will enter a full system lockout to protect the HVAC from major damage, requiring a manual reset.
* **Sensor Fault Detection**: Validates that sensor readings are within a plausible range. If a fault is detected, the system enters a safe fault state, shutting down all relays.
* **Hardware Watchdog Timer**: Utilizes the ESP32's built-in watchdog to automatically reboot the device if the main code ever freezes, ensuring the system never gets stuck with a relay on.
* **Power-On Delay**: Waits for a few seconds after booting before starting any HVAC operations to protect against power surges.
* **Persistent Storage (NVS)**: All user settings, including the full schedule and learned performance data, are saved to the ESP32's Non-Volatile Storage, ensuring all configurations are retained through power outages.

## Software Architecture

The project is organized into a clean, modular, multi-file structure to separate concerns and improve maintainability.

### `main.cpp` / `main.h`
* **Role**: The main application entry point and coordinator.
* **Responsibilities**:
  * Initializes all hardware and software modules (`Serial`, NVS, Watchdog, Filesystem).
  * Loads settings from NVS on boot.
  * Runs the main `loop()`, which orchestrates calls to all other modules.
  * Handles top-level logic like sensor fault detection and smart recovery calculations.
  * Contains the hardware abstraction layer (e.g., `readRelay`, `writeRelay`) to facilitate testing.

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
1. **IDE**: This project is designed to be built with an IDE that supports the ESP-IDF framework, such as **VS Code with the PlatformIO extension**.
2. **File Structure**: In PlatformIO, place `.h` files in the `include` directory and `.cpp` files in the `src` directory.
3. **Libraries**: No external libraries are required beyond what is included with the standard ESP-IDF.
4. **Configuration**: Modify the settings in `include/config.h` to match your HVAC system.
5. **Build & Upload**: Use PlatformIO to build and upload the project to your ESP32-C6 board.
6. **Monitor**: Open the Serial Monitor at `115200` baud to view the test suite results and status logs.