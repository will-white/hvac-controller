#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

// -- Temperature Unit Selection --
enum TempUnit { CELSIUS, FAHRENHEIT };

// -- System & Fan Mode Enums --
enum SystemMode { SYS_OFF, SYS_HEAT, SYS_COOL, SYS_AUTO };
enum FanMode { FAN_AUTO, FAN_ON, FAN_CIRCULATE };

// -- Data Structures --
struct TimeInfo {
  int month;
  int day;
  int dayOfWeek; // 0=Sun, 1=Mon, ..., 6=Sat
  int hour;
  int minute;
};

// Every schedule period carries a heat-to / cool-to pair (F). Heat mode uses
// only the heat setpoint, cool mode only the cool one, auto uses both with an
// enforced minimum gap so the stages can never fight over one number.
struct ScheduleEntry {
  int startHour;
  int startMinute;
  float heatSetpointF;
  float coolSetpointF;
  FanMode fanMode;
};

struct Schedule {
  ScheduleEntry weekday[10] = {
    {  6, 30, 70.0, 75.0, FAN_AUTO },      // Wake
    {  8, 30, 64.0, 79.0, FAN_CIRCULATE }, // Away
    { 17, 30, 72.0, 75.0, FAN_AUTO },      // Home
    { 22,  0, 68.0, 74.0, FAN_AUTO },      // Sleep
  };
  int weekdayEntryCount = 4;

  ScheduleEntry weekend[10] = {
    {  8,  0, 71.0, 76.0, FAN_AUTO },
    { 23,  0, 69.0, 74.0, FAN_AUTO },
  };
  int weekendEntryCount = 2;

  // Wide protective band while away: heat only to prevent freezing,
  // cool only to protect the house from extreme heat.
  ScheduleEntry vacation = { 0, 0, 55.0, 85.0, FAN_CIRCULATE };
};

// Where the currently active targets came from — surfaced by the console so
// "why is it 68 in here?" always has an answer.
enum TargetSource {
  SRC_SCHEDULE,          // a schedule entry that started today
  SRC_CARRYOVER,         // previous day's last entry (before today's first)
  SRC_RECOVERY,          // smart recovery: pre-heating/cooling toward the NEXT entry
  SRC_VACATION,          // vacation hold
  SRC_HOLD,              // temporary user hold (console/UI)
  SRC_FREEZE_PROTECT,    // indoor dangerously cold -> heat forced on
  SRC_HEAT_PROTECT,      // indoor dangerously hot -> cooling forced on
  SRC_NO_TIME_FALLBACK,  // clock never synced -> safe hold, schedule unusable
};

struct ResolvedTargets {
  float heatSetpointF;
  float coolSetpointF;
  FanMode fanMode;
  SystemMode effectiveMode; // protections may override the user's mode
  TargetSource source;
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

// -- Relay Assignments --
// Hardware: Olimex ESP32-C6-EVB (4 onboard relays, 10A/240VAC, GPIO-driven)
//         + Olimex MOD-IO2 Rev.B on UEXT (2 relays, 15A/250VAC, I2C-driven).
// These are LOGICAL relay indices — the physical mapping (GPIO vs MOD-IO2
// channel) lives in relays.cpp. The four 24VAC control signals critical to
// heating/cooling stay on the EVB's native relays so an I2C fault can never
// strand them; the humidifier rides the MOD-IO2.
const int HEATER_RELAY              = 0; // EVB REL1 (GPIO10) — W
const int COOLER_RELAY              = 1; // EVB REL2 (GPIO11) — Y
const int FAN_RELAY                 = 2; // EVB REL3 (GPIO22) — G
const int FRESH_AIR_RELAY           = 3; // EVB REL4 (GPIO23) — damper
const int HUMIDITY_RELAY            = 4; // MOD-IO2 relay 1
const int SPARE_RELAY               = 5; // MOD-IO2 relay 2 (unassigned)
const int RELAY_COUNT               = 6;

// -- UEXT / MOD-IO2 bus --
// ESP32-C6-EVB UEXT: SDA=GPIO6, SCL=GPIO7 (per Olimex Rev.A schematic).
// MOD-IO2 Rev.B: default address 0x21, firmware 3.xx/4.3 command set.
// NOTE: MOD-IO2 needs its own 12V DC on its barrel jack to actuate relays.
const int     UEXT_I2C_SDA_PIN      = 6;
const int     UEXT_I2C_SCL_PIN      = 7;
const uint8_t MODIO2_I2C_ADDRESS    = 0x21;

// -- Sensors --
// Primary indoor sensing: REMOTE wireless probes (Zigbee Aqara planned),
// injected via sensors_injectRemote(). A reading older than the staleness
// window is treated as absent (Aqara heartbeat is ~1h, so 90 min covers a
// single missed report before the system falls back / faults).
const unsigned long REMOTE_SENSOR_STALE_MINS = 90;
// Optional local fallback: Sensirion SHT4x (temp + RH) on UEXT I2C, addr 0x44.
const uint8_t SHT4X_I2C_ADDRESS     = 0x44;

// -- ESP32-C6-EVB other I/O --
// User/boot button BUT1=GPIO9, UART0 (ESP-PROG)=GPIO16/17.

// -- Opto-isolated inputs (<=30V): equipment feedback & contact sense --
// 24VAC through the opto conducts only on positive half-cycles, so a live
// signal reads as ~60Hz pulses — "active" means asserted within the last
// OPTO_AC_HOLD_MS, bridging the zero crossings. An input that has NEVER been
// seen active is "not commissioned" (not wired yet) and all its rules stay
// quiet; first signal arms it (mask persisted with the health counters).
enum OptoRole { OPTO_NONE, OPTO_HEAT_PROOF, OPTO_COOL_PROOF, OPTO_FAN_PROOF, OPTO_WINDOW };
const int      OPTO_COUNT             = 4;
const int      OPTO_PINS[OPTO_COUNT]  = { 1, 2, 3, 15 }; // OPT1..OPT4
const OptoRole OPTO_ROLES[OPTO_COUNT] = { OPTO_HEAT_PROOF, OPTO_COOL_PROOF, OPTO_FAN_PROOF, OPTO_WINDOW };
const bool     OPTO_ACTIVE_LOW        = true; // opto transistor pulls the GPIO low when energized
const unsigned long OPTO_AC_HOLD_MS        = 100;  // hold "active" across AC zero crossings
const unsigned long OPTO_PROVE_GRACE_SECS  = 90;   // call must show feedback within this
const unsigned long OPTO_PHANTOM_GRACE_SECS = 300; // feedback w/o call this long = welded relay
                                                   // (long: furnaces run their own blower delays)

// -- Status LED (EVB user LED, blink-codes for fault classes) --

// -- Status LED (EVB user LED, blink-codes for fault classes) --
const int  STATUS_LED_PIN         = 8;    // strapping pin; safe to drive after boot
const bool STATUS_LED_ACTIVE_HIGH = true; // flip if the EVB LED proves inverted

// -- Network / Time --
// POSIX TZ string; default is US Eastern — change for your locale.
const char TZ_INFO[]                        = "EST5EDT,M3.2.0,M11.1.0";
const char NTP_SERVER_1[]                   = "pool.ntp.org";
const char NTP_SERVER_2[]                   = "time.nist.gov";
const unsigned long WIFI_RETRY_INTERVAL_MS  = 30UL * 1000UL;

// -- Setpoint Resolution & Protection --
const float         MIN_SETPOINT_GAP_F      = 4.0;  // enforced heat/cool gap in AUTO
const float         FREEZE_PROTECT_TRIGGER_F = 45.0; // force heat below this...
const float         FREEZE_PROTECT_TARGET_F  = 48.0; // ...until back up to this
const float         HEAT_PROTECT_TRIGGER_F   = 92.0; // force cool above this...
const float         HEAT_PROTECT_TARGET_F    = 88.0; // ...until back down to this
const float         NO_TIME_FALLBACK_HEAT_F  = 68.0; // safe hold when clock unknown
const float         NO_TIME_FALLBACK_COOL_F  = 76.0;
const unsigned long DEFAULT_HOLD_HOURS       = 2;

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
// Circulate = guarantee this many fan-minutes per rolling hour; time the fan
// runs anyway (heat/cool/damper/purge) counts toward the quota.
const unsigned long FAN_CIRCULATE_MINS_PER_HOUR     = 15;
// Post-heat fan purge: harvest residual heat-exchanger warmth. (No cool-side
// purge: running the fan after cooling re-evaporates coil condensate.)
const unsigned long FAN_OFF_DELAY_AFTER_HEAT_SECS   = 90;

// -- Safety Lockout & Fault Settings --
const int           MAX_RUN_TRIGGER_COUNT           = 3;
const unsigned long MAX_RUN_LOCKOUT_HOURS           = 4;
const float         MIN_PLAUSIBLE_TEMP_F            = 0.0;
const float         MAX_PLAUSIBLE_TEMP_F            = 120.0;
const float         MIN_PLAUSIBLE_OUTDOOR_TEMP_F    = -40.0;
const float         MAX_PLAUSIBLE_OUTDOOR_TEMP_F    = 130.0;
const unsigned long WDT_TIMEOUT_SECS                = 10;

// -- Control Loop & Console --
const unsigned long CONTROL_INTERVAL_MS             = 5000;
const unsigned long TEST_MODE_RELAY_TIMEOUT_MS      = 5UL * 60UL * 1000UL;  // relay auto-off in service mode
const unsigned long TEST_MODE_IDLE_TIMEOUT_MS       = 30UL * 60UL * 1000UL; // service mode auto-exit

// -- Persistence --
// Bump whenever the layout of any NVS-persisted struct changes; stale blobs
// are discarded (defaults win) instead of being silently mis-loaded.
const uint8_t       SETTINGS_SCHEMA_VERSION         = 2;

// -- Health / S.M.A.R.T.-style detection rules (advisory: log + report only) --
const unsigned long HEALTH_NO_RESPONSE_GRACE_MINS   = 10;   // call active this long...
const float         HEALTH_NO_RESPONSE_DELTA_F      = 0.5;  // ...and temp moved this far the WRONG way
const float         HEALTH_CAPACITY_WARN_FRACTION   = 0.6;  // cycle rate below this x learned baseline
const int           HEALTH_CAPACITY_MIN_SAMPLES     = 5;    // baseline must have this many samples
const int           HEALTH_SHORT_CYCLES_PER_HOUR    = 4;    // starts within a rolling hour
const unsigned long FILTER_SERVICE_FAN_HOURS        = 300;  // blower hours between filter changes

// -- Learning Algorithm Settings --
// (array, not pointer: a non-const pointer at namespace scope has external
// linkage and causes multiple-definition link errors across translation units)
static const char   LEARNING_LOG_FILE[]             = "/learning_log.csv";
const int           MIN_ADJUSTMENTS_TO_LEARN        = 3;
const unsigned long ADJUSTMENT_ANALYSIS_HOURS       = 24;

// -- Time to Temperature (Smart Recovery) Settings --
const bool          ENABLE_SMART_RECOVERY           = true;
const int           MAX_RECOVERY_TIME_MINS          = 180;
// Used until the learner has a sample for the current season/outdoor bin.
const float         DEFAULT_RECOVERY_RATE_F_PER_HR  = 3.0;

// -- Humidity Control --
const bool          HUMIDIFIER_REQUIRES_AIRFLOW     = true;
// Overcool-to-dehumidify: when RH exceeds the threshold, allow cooling up to
// this far below the cool setpoint (A/C is the only dehumidifier we have).
const bool          ENABLE_OVERCOOL_DEHUMIDIFY      = true;
const float         DEHUMIDIFY_RH_THRESHOLD         = 55.0;
const float         DEHUMIDIFY_OVERCOOL_MAX_F       = 1.5;

// -- Weather (outdoor temperature via open-meteo; coordinates in secrets.h) --
// Outdoor priority: a LOCAL probe (Zigbee via Zigbee2MQTT, Phase 4) always
// beats the regional weather API while its report is fresh.
const unsigned long WEATHER_POLL_MINS               = 10;
const unsigned long OUTDOOR_STALE_MINS              = 30;   // weather-API staleness
// (outdoor probe staleness reuses REMOTE_SENSOR_STALE_MINS)

// -- Persistence & flash wear --
// Learned data flushes to NVS on a timer, not per-cycle: NVS is log-structured
// (writes append, sectors erase on GC), and batching keeps erases to a handful
// per week. Losing <=6h of averaged samples on a power cut is noise.
const unsigned long PERF_FLUSH_INTERVAL_MINS        = 360;
const unsigned long LEARNING_LOG_MAX_BYTES          = 65536; // rotate CSV beyond this

// -- Event journal (flash-persisted copy of the RAM event ring) --
static const char   EVENT_JOURNAL_FILE[]            = "/event_journal.log";
static const char   EVENT_JOURNAL_OLD_FILE[]        = "/event_journal.old"; // one retained generation
const unsigned long EVENT_JOURNAL_MAX_BYTES         = 49152; // rotate current -> .old beyond this
const int           EVENT_JOURNAL_MAX_PER_MIN       = 30;    // flash-wear cap (`sim speed` storms)

#endif // CONFIG_H
