#include "sensors.h"
#include <Arduino.h>
#include <Wire.h>
#include <cmath>
#include "config.h"
#include "console.h"
#include "events.h"
#include "hvac_tests.h"
#include "main.h"
#include "utils.h"

// -- Remote (wireless) probe ingestion --
// Values arrive asynchronously (Zigbee report-on-change + periodic heartbeat).
// Staleness runs on REAL millis: sim speed must not expire a live probe.
static float remoteTempF = NAN;
static float remoteRh = NAN;
static unsigned long remoteLastReportMs = 0;
static bool remoteEverReported = false;

void sensors_injectRemote(float tempF, float rhPct) {
  remoteTempF = tempF;
  remoteRh = rhPct;
  remoteLastReportMs = millis();
  remoteEverReported = true;
  logEvent("remote report: %.1fF %.0f%%RH", tempF, rhPct);
}

bool sensors_remoteFresh() {
  return remoteEverReported &&
         (millis() - remoteLastReportMs) < REMOTE_SENSOR_STALE_MINS * 60000UL;
}

// -- Outdoor feeds: local probe (preferred) and weather API (fallback) --
static float outdoorProbeF = NAN;
static unsigned long outdoorProbeMs = 0;
static bool outdoorProbeEver = false;
static float outdoorWeatherF = NAN;
static unsigned long outdoorWeatherMs = 0;
static bool outdoorWeatherEver = false;

void sensors_injectOutdoorProbe(float tempF) {
  outdoorProbeF = tempF;
  outdoorProbeMs = millis();
  if (!outdoorProbeEver) logEvent("outdoor probe online: %.1fF", tempF);
  outdoorProbeEver = true;
}

void sensors_injectOutdoorWeather(float tempF) {
  outdoorWeatherF = tempF;
  outdoorWeatherMs = millis();
  if (!outdoorWeatherEver) logEvent("outdoor weather feed online: %.1fF", tempF);
  outdoorWeatherEver = true;
}

static bool outdoorProbeFresh() {
  return outdoorProbeEver && !isnan(outdoorProbeF) &&
         (millis() - outdoorProbeMs) < REMOTE_SENSOR_STALE_MINS * 60000UL;
}

static bool outdoorWeatherFresh() {
  return outdoorWeatherEver && !isnan(outdoorWeatherF) &&
         (millis() - outdoorWeatherMs) < OUTDOOR_STALE_MINS * 60000UL;
}

const char* sensors_outdoorSource() {
  if (outdoorProbeFresh()) return "probe";
  if (outdoorWeatherFresh()) return "weather";
  return "none";
}

// -- SHT4x driver (optional local fallback; no external library) --
// Command 0xFD = high-precision measure; ~9 ms later read 6 bytes:
// [tempHi tempLo crc] [rhHi rhLo crc], CRC-8 poly 0x31 init 0xFF.
static bool sht4xPresent = false;
static bool sht4xEverRead = false;
static int consecutiveFailures = 0;
static float lastTempC = NAN;
static float lastRh = NAN;

static uint8_t crc8(const uint8_t* data, int len) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
  }
  return crc;
}

static bool sht4xMeasure() {
  Wire.beginTransmission(SHT4X_I2C_ADDRESS);
  Wire.write(0xFD);
  if (Wire.endTransmission() != 0) return false;
  delay(10); // high-precision conversion time
  if (Wire.requestFrom((int)SHT4X_I2C_ADDRESS, 6) != 6) return false;
  uint8_t d[6];
  for (int i = 0; i < 6; i++) d[i] = Wire.read();
  if (crc8(d, 2) != d[2] || crc8(d + 3, 2) != d[5]) return false;
  uint16_t rawT = ((uint16_t)d[0] << 8) | d[1];
  uint16_t rawH = ((uint16_t)d[3] << 8) | d[4];
  lastTempC = -45.0f + 175.0f * rawT / 65535.0f;
  lastRh = -6.0f + 125.0f * rawH / 65535.0f;
  if (lastRh < 0) lastRh = 0;
  if (lastRh > 100) lastRh = 100;
  sht4xEverRead = true;
  return true;
}

// Refresh at most once per control tick; both readers share one measurement.
static void refresh() {
  if (!sht4xPresent) return;
  if (sht4xMeasure()) {
    consecutiveFailures = 0;
  } else if (++consecutiveFailures >= 3) {
    if (!isnan(lastTempC)) logEvent("SHT4x unresponsive (3 consecutive failures)");
    lastTempC = NAN;
    lastRh = NAN;
  }
}

void sensors_begin() {
  // Probe: soft-reset command answered = sensor present.
  Wire.beginTransmission(SHT4X_I2C_ADDRESS);
  Wire.write(0x94);
  sht4xPresent = (Wire.endTransmission() == 0);
  if (sht4xPresent) {
    delay(2);
    Serial.println("Local SHT4x fallback sensor detected on UEXT I2C 0x44.");
  } else {
    Serial.println("No local SHT4x (optional). Primary sensing is remote probe reports;"
                   " on the bench use console 'remote <tempF>' or 'sim temp <F>'.");
  }
}

bool sensors_indoorPresent() { return sht4xPresent; }

bool sensors_hasEverHadData() { return remoteEverReported || sht4xEverRead; }

float readTemperature() {
  if (g_isTesting) return g_mockIndoorTempF;
  float simF;
  if (console_simTemp(simF)) return (tempUnit == FAHRENHEIT) ? simF : fahrenheitToCelsius(simF);
  if (sensors_remoteFresh() && !isnan(remoteTempF)) {
    return (tempUnit == FAHRENHEIT) ? remoteTempF : fahrenheitToCelsius(remoteTempF);
  }
  refresh();
  if (isnan(lastTempC)) return NAN;
  return (tempUnit == FAHRENHEIT) ? celsiusToFahrenheit(lastTempC) : lastTempC;
}

float readOutdoorTemperature() {
  if (g_isTesting) return g_mockOutdoorTempF;
  float simF;
  if (console_simOutdoor(simF)) return (tempUnit == FAHRENHEIT) ? simF : fahrenheitToCelsius(simF);
  if (outdoorProbeFresh()) return (tempUnit == FAHRENHEIT) ? outdoorProbeF : fahrenheitToCelsius(outdoorProbeF);
  if (outdoorWeatherFresh()) return (tempUnit == FAHRENHEIT) ? outdoorWeatherF : fahrenheitToCelsius(outdoorWeatherF);
  // No fresh outdoor data: NAN keeps the economizer honestly disabled
  // (main.cpp substitutes the indoor reading, neutralizing the comparisons).
  return NAN;
}

float readHumidity() {
  float simRh;
  if (console_simHumidity(simRh)) return simRh;
  if (g_isTesting) return 45.0; // benign fixed value for logic tests
  if (sensors_remoteFresh() && !isnan(remoteRh)) return remoteRh;
  // refresh() already ran via readTemperature() this tick; don't double-measure.
  return lastRh;
}
