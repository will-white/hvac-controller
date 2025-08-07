#include <Arduino.h>
#include "config.h"
#include "main.h"
#include "hvac_logic.h"
#include "hvac_tests.h"
#include "learning.h"
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

float currentTargetTemperature;
FanMode currentFanMode;
enum ThermostatState { IDLE, RECOVERING, HEATING, COOLING, FAN_ONLY };
ThermostatState currentState = IDLE;

// -- Mocking infrastructure for tests --
bool g_isTesting = false;
unsigned long g_mockMillis = 0;
TimeInfo g_mockTime;
float g_mockIndoorTempF, g_mockOutdoorTempF;
bool g_mockRelayStates[10] = {LOW,LOW,LOW,LOW,LOW,LOW,LOW,LOW,LOW,LOW};

// =================================================================
// ==              HARDWARE ABSTRACTION & UTILITIES               ==
// =================================================================
float fahrenheitToCelsius(float f) { return (f - 32.0) * 5.0 / 9.0; }
float celsiusToFahrenheit(float c) { return (c * 9.0 / 5.0) + 32.0; }
void writeRelay(int p, bool v) { if (g_isTesting) g_mockRelayStates[p] = v; else digitalWrite(p, v); }
bool readRelay(int p) { return g_isTesting ? g_mockRelayStates[p] : digitalRead(p); }
unsigned long currentTime() { return g_isTesting ? g_mockMillis : millis(); }

// =================================================================
// ==                  NVS SAVE/LOAD FUNCTIONS                    ==
// =================================================================
void saveSettings() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("hvac_cfg", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) { Serial.printf("Error (%s) opening NVS handle for writing!\n", esp_err_to_name(err)); return; }

    nvs_set_blob(my_handle, "schedule", &programSchedule, sizeof(programSchedule));
    nvs_set_blob(my_handle, "perf", &performance, sizeof(performance));
    nvs_set_i8(my_handle, "tempUnit", (int8_t)tempUnit);
    nvs_set_i8(my_handle, "systemMode", (int8_t)systemMode);
    nvs_set_i8(my_handle, "vacation", (int8_t)vacationModeActive);
    
    nvs_commit(my_handle);
    nvs_close(my_handle);
}

void loadSettings() {
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("hvac_cfg", NVS_READONLY, &my_handle);
    if (err != ESP_OK) { saveSettings(); return; }

    size_t required_size = sizeof(programSchedule);
    nvs_get_blob(my_handle, "schedule", &programSchedule, &required_size);
    
    size_t perf_size = sizeof(performance);
    nvs_get_blob(my_handle, "perf", &performance, &perf_size);

    int8_t temp_i8;
    if(nvs_get_i8(my_handle, "tempUnit", &temp_i8) == ESP_OK) tempUnit = (TempUnit)temp_i8;
    if(nvs_get_i8(my_handle, "systemMode", &temp_i8) == ESP_OK) systemMode = (SystemMode)temp_i8;
    if(nvs_get_i8(my_handle, "vacation", &temp_i8) == ESP_OK) vacationModeActive = (bool)temp_i8;

    nvs_close(my_handle);
}

// =================================================================
// ==                 SCHEDULING & CORE LOGIC                     ==
// =================================================================
TimeInfo getCurrentTime() {
  if (g_isTesting) return g_mockTime;
  // This function MUST be implemented with a real RTC or NTP client.
  TimeInfo now;
  now.month = 8; now.day = 7; now.dayOfWeek = 4; now.hour = 9; now.minute = 52;
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

void getCurrentScheduleSettings() {
  if (vacationModeActive) {
    currentTargetTemperature = programSchedule.vacation.targetTemperature;
    currentFanMode = programSchedule.vacation.fanMode;
    return;
  }
  
  TimeInfo now = getCurrentTime();
  ScheduleEntry* daySchedule;
  int entryCount;

  if (now.dayOfWeek >= 1 && now.dayOfWeek <= 5) { daySchedule = programSchedule.weekday; entryCount = programSchedule.weekdayEntryCount; } 
  else { daySchedule = programSchedule.weekend; entryCount = programSchedule.weekendEntryCount; }

  ScheduleEntry activeEntry = daySchedule[0];
  for (int i = 0; i < entryCount; i++) {
    if (now.hour > daySchedule[i].startHour || (now.hour == daySchedule[i].startHour && now.minute >= daySchedule[i].startMinute)) {
      activeEntry = daySchedule[i];
    }
  }
  currentTargetTemperature = activeEntry.targetTemperature;
  currentFanMode = activeEntry.fanMode;
}

bool validateSensorReadings(float indoorTempF) {
    if (indoorTempF < MIN_PLAUSIBLE_TEMP_F || indoorTempF > MAX_PLAUSIBLE_TEMP_F) {
        Serial.printf("CRITICAL FAULT: Indoor temperature reading (%.1f F) is outside plausible range!\n", indoorTempF);
        return false;
    }
    return true;
}

// =================================================================
// ==                     SETUP & LOOP                            ==
// =================================================================
void setup() {
  Serial.begin(115200);
  while (!Serial);

  esp_task_wdt_init(10, true); 
  esp_task_wdt_add(NULL);

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  
  loadSettings(); 
  initialize_learning();
  runTests();

  pinMode(HEATER_RELAY_PIN, OUTPUT); pinMode(COOLER_RELAY_PIN, OUTPUT);
  pinMode(FAN_RELAY_PIN, OUTPUT);    pinMode(FRESH_AIR_RELAY_PIN, OUTPUT);
  pinMode(HUMIDITY_RELAY_PIN, OUTPUT);

  initialize_logic_timers();
  
  Serial.println("Initialization Complete. Applying power-on delay...");
  delay(5000); 
  Serial.println("Starting control loop.");
}

void loop() {
  esp_task_wdt_reset();

  if (systemInFaultState) {
    Serial.println("System in FAULT state. Manual reset required. Halting operations.");
    writeRelay(HEATER_RELAY_PIN, LOW); writeRelay(COOLER_RELAY_PIN, LOW);
    writeRelay(FAN_RELAY_PIN, LOW);    writeRelay(FRESH_AIR_RELAY_PIN, LOW);
    writeRelay(HUMIDITY_RELAY_PIN, LOW);
    delay(30000);
    return;
  }

  getCurrentScheduleSettings();

  float currentTempRaw = readTemperature();
  float outdoorTempRaw = readOutdoorTemperature();
  float currentHumidity = readHumidity();

  if (!validateSensorReadings( (tempUnit == FAHRENHEIT) ? currentTempRaw : celsiusToFahrenheit(currentTempRaw) )) {
    systemInFaultState = true;
    return; 
  }
  
  analyze_and_learn_if_needed();

  float currentTempC = (tempUnit == FAHRENHEIT) ? fahrenheitToCelsius(currentTempRaw) : currentTempRaw;
  float outdoorTempC = (tempUnit == FAHRENHEIT) ? fahrenheitToCelsius(outdoorTempRaw) : outdoorTempRaw;
  float targetTempC = (tempUnit == FAHRENHEIT) ? fahrenheitToCelsius(currentTargetTemperature) : currentTargetTemperature;
  
  char unitChar = (tempUnit == FAHRENHEIT) ? 'F' : 'C';
  TimeInfo now = getCurrentTime();
  Serial.print("Time: "); Serial.print(now.hour); Serial.print(":"); if(now.minute < 10) Serial.print("0"); Serial.print(now.minute);
  Serial.print(" | Temp: "); Serial.print(currentTempRaw, 1); Serial.print(unitChar);
  Serial.print(", Target: "); Serial.print(currentTargetTemperature, 1); Serial.print(unitChar);
  Serial.print(" | Humidity: "); Serial.print(currentHumidity, 1); Serial.println("%");

  controlTemperature(currentTempC, targetTempC, outdoorTempC);
  controlHumidity(currentHumidity, currentTempC, outdoorTempC);
  controlFan(currentFanMode);
  
  delay(5000);
}