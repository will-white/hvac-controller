#ifndef SENSORS_H
#define SENSORS_H

// Sensor abstraction. Indoor read order per value: unit-test mock
// (g_isTesting) → console sim override → fresh remote report → local SHT4x
// (optional fallback) → NAN. NAN is the honest "no data" answer; the control
// layer distinguishes "never commissioned" (waits quietly) from "sensor died"
// (faults).
void sensors_begin();
bool sensors_indoorPresent();     // local SHT4x detected
bool sensors_hasEverHadData();    // any indoor source has ever produced a reading

// Transport-agnostic ingestion for remote wireless probes (Zigbee, MQTT,
// ESP-NOW — whatever lands later calls this; console `remote` emulates it).
// tempF in Fahrenheit; pass NAN for rhPct if the probe has no humidity.
void sensors_injectRemote(float tempF, float rhPct);
bool sensors_remoteFresh();       // a remote report exists and is not stale

// Outdoor feeds, priority-ordered: a fresh LOCAL probe report (Zigbee via
// Zigbee2MQTT later; console `outdoor` today) always outranks the weather API.
void sensors_injectOutdoorProbe(float tempF);   // staleness: REMOTE_SENSOR_STALE_MINS
void sensors_injectOutdoorWeather(float tempF); // staleness: OUTDOOR_STALE_MINS
const char* sensors_outdoorSource();            // "probe" | "weather" | "none" (diagnostics)

float readTemperature();          // indoor, in DEFAULT_TEMP_UNIT
float readOutdoorTemperature();   // NAN until outdoor hardware exists (Phase 3: weather API)
float readHumidity();             // indoor %RH

#endif // SENSORS_H
