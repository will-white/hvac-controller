#ifndef CONFIG_H
#define CONFIG_H

// -- Temperature Unit Selection --
enum TempUnit { CELSIUS, FAHRENHEIT };

// -- System & Fan Mode Enums --
enum SystemMode { SYS_OFF, SYS_HEAT, SYS_COOL, SYS_AUTO };
enum FanMode { FAN_AUTO, FAN_ON, FAN_CIRCULATE };

// -- Data Structures --
struct ScheduleEntry {
  int startHour; 
  int startMinute; 
  float targetTemperature; 
  FanMode fanMode;
};

struct Schedule {
  ScheduleEntry weekday[10] = {
    { 6, 30, 70.0, FAN_AUTO },      
    { 8, 30, 75.0, FAN_CIRCULATE }, 
    { 17, 30, 72.0, FAN_AUTO },     
    { 22, 00, 68.0, FAN_AUTO },     
  };
  int weekdayEntryCount = 4;

  ScheduleEntry weekend[10] = { 
    { 8, 00, 71.0, FAN_AUTO }, 
    { 23, 00, 69.0, FAN_AUTO } 
  };
  int weekendEntryCount = 2;
  
  ScheduleEntry vacation = { 0, 0, 80.0, FAN_CIRCULATE };
};

#define NUM_PERFORMANCE_BINS 8
#define NUM_SEASONS 4 // 0:Winter, 1:Spring, 2:Summer, 3:Fall

struct HvacPerformance {
  // heatRate[season][temperature_bin]
  float heatRate[NUM_SEASONS][NUM_PERFORMANCE_BINS] = {{0}};
  int   heatSamples[NUM_SEASONS][NUM_PERFORMANCE_BINS] = {{0}};
  
  float coolRate[NUM_SEASONS][NUM_PERFORMANCE_BINS] = {{0}};
  int   coolSamples[NUM_SEASONS][NUM_PERFORMANCE_BINS] = {{0}};
};

// =================================================================
// ==                      CONFIGURATION                          ==
// =================================================================
const TempUnit      DEFAULT_TEMP_UNIT       = FAHRENHEIT;
const SystemMode    DEFAULT_SYSTEM_MODE     = SYS_AUTO;
const float         HUMIDITY_TARGET         = 45.0;

// -- Relay Pin Assignments --
const int HEATER_RELAY_PIN          = 2;
const int COOLER_RELAY_PIN          = 3;
const int FAN_RELAY_PIN             = 4;
const int FRESH_AIR_RELAY_PIN       = 5;
const int HUMIDITY_RELAY_PIN        = 6;

// -- Hysteresis, Cycle, and Protection Settings --
const float         TEMPERATURE_DEADBAND_F          = 1.0;
const float         HUMIDITY_DEADBAND               = 2.0;
const unsigned long MIN_HEATER_RUN_TIME_MINS        = 3;
const unsigned long MIN_HEATER_OFF_TIME_MINS        = 5;
const unsigned long MAX_HEATER_RUN_TIME_MINS        = 90;
const unsigned long MIN_COOLER_RUN_TIME_MINS        = 4;
const unsigned long MIN_COOLER_OFF_TIME_MINS        = 5;
const unsigned long MAX_COOLER_RUN_TIME_MINS        = 90;
const float         WINDOW_EFFICIENCY_FACTOR        = 0.6; 
const float         FRESH_AIR_TEMP_DIFFERENTIAL_F   = 4.0;
const unsigned long FAN_CIRCULATE_ON_TIME_MINS      = 15;
const unsigned long FAN_CIRCULATE_OFF_TIME_MINS     = 45;

// -- Safety Lockout & Fault Settings --
const int           MAX_RUN_TRIGGER_COUNT           = 3;
const unsigned long MAX_RUN_LOCKOUT_HOURS           = 4;
const float         MIN_PLAUSIBLE_TEMP_F            = 0.0;
const float         MAX_PLAUSIBLE_TEMP_F            = 120.0;

// -- Learning Algorithm Settings --
const char* LEARNING_LOG_FILE               = "/learning_log.csv";
const int           MIN_ADJUSTMENTS_TO_LEARN        = 3;
const unsigned long ADJUSTMENT_ANALYSIS_HOURS       = 24;

// -- Time to Temperature (Smart Recovery) Settings --
const bool          ENABLE_SMART_RECOVERY           = true;
const int           MAX_RECOVERY_TIME_MINS          = 180;

#endif // CONFIG_H