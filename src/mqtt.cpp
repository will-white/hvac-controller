#include "mqtt.h"
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <cmath>
#include <cstring>
#include "config.h"
#include "events.h"
#include "fw_version.h"
#include "health.h"
#include "main.h"
#include "net_time.h"
#include "relays.h"
#include "sensors.h"
#include "setpoints.h"
#include "utils.h"
#include "netconfig.h"

static WiFiClient wifiClient;
static PubSubClient client(wifiClient);
static char deviceId[24] = "";
static char baseTopic[40] = "";
static unsigned long lastAttemptMs = 0;
static unsigned long lastStateMs = 0;
static bool everConnected = false;
static bool connectFailLogged = false;

static bool enabled() { return netcfg().mqttHost[0] != '\0'; }
bool mqtt_connected() { return enabled() && client.connected(); }

static const char* modeName(SystemMode m) {
  switch (m) { case SYS_OFF: return "off"; case SYS_HEAT: return "heat";
               case SYS_COOL: return "cool"; default: return "auto"; }
}

static void pub(const char* suffix, const char* payload, bool retained = false) {
  char t[64];
  snprintf(t, sizeof(t), "%s/%s", baseTopic, suffix);
  client.publish(t, payload, retained);
}

static void pubFloat(const char* suffix, float v) {
  if (isnan(v)) return; // HA treats a missing topic better than "nan"
  char p[16];
  snprintf(p, sizeof(p), "%.1f", v);
  pub(suffix, p);
}

static void publishState() {
  lastStateMs = millis();
  pubFloat("temp", lastIndoorF);
  pubFloat("rh", readHumidity());
  pubFloat("outdoor", readOutdoorTemperature());
  pubFloat("heat_sp", lastResolved.heatSetpointF);
  pubFloat("cool_sp", lastResolved.coolSetpointF);
  pub("mode", modeName(systemMode));
  pub("source", targetSourceName(lastResolved.source));
  const char* action = "idle";
  if (systemInFaultState || systemMode == SYS_OFF) action = "off";
  else if (readRelay(HEATER_RELAY)) action = "heating";
  else if (readRelay(COOLER_RELAY)) action = "cooling";
  else if (readRelay(FAN_RELAY)) action = "fan";
  pub("action", action);
  char h[8];
  snprintf(h, sizeof(h), "%d", health_warningCount());
  pub("health", h);
}

static void publishDiscovery() {
  static char buf[1200];
  char cfgTopic[80];

  // Climate entity (dual setpoints, F).
  snprintf(cfgTopic, sizeof(cfgTopic), "homeassistant/climate/%s/config", deviceId);
  snprintf(buf, sizeof(buf),
    "{\"name\":\"Thermostat\",\"uniq_id\":\"%s\",\"~\":\"%s\","
    "\"modes\":[\"off\",\"heat\",\"cool\",\"auto\"],"
    "\"mode_cmd_t\":\"~/cmd/mode\",\"mode_stat_t\":\"~/mode\","
    "\"temp_lo_cmd_t\":\"~/cmd/heat_sp\",\"temp_lo_stat_t\":\"~/heat_sp\","
    "\"temp_hi_cmd_t\":\"~/cmd/cool_sp\",\"temp_hi_stat_t\":\"~/cool_sp\","
    "\"curr_temp_t\":\"~/temp\",\"curr_hum_t\":\"~/rh\",\"act_t\":\"~/action\","
    "\"avty_t\":\"~/availability\",\"min_temp\":40,\"max_temp\":95,"
    "\"temp_step\":0.5,\"temperature_unit\":\"F\","
    "\"dev\":{\"ids\":[\"%s\"],\"name\":\"HVAC Controller\",\"mf\":\"DIY\",\"mdl\":\"Olimex ESP32-C6-EVB\","
    "\"sw\":\"%s\"}}",
    deviceId, baseTopic, deviceId, FW_VERSION);
  client.publish(cfgTopic, buf, true);

  // Outdoor temperature sensor.
  snprintf(cfgTopic, sizeof(cfgTopic), "homeassistant/sensor/%s_outdoor/config", deviceId);
  snprintf(buf, sizeof(buf),
    "{\"name\":\"Outdoor Temperature\",\"uniq_id\":\"%s_outdoor\","
    "\"stat_t\":\"%s/outdoor\",\"unit_of_meas\":\"°F\",\"dev_cla\":\"temperature\","
    "\"avty_t\":\"%s/availability\",\"dev\":{\"ids\":[\"%s\"]}}",
    deviceId, baseTopic, baseTopic, deviceId);
  client.publish(cfgTopic, buf, true);

  // Health warning count.
  snprintf(cfgTopic, sizeof(cfgTopic), "homeassistant/sensor/%s_health/config", deviceId);
  snprintf(buf, sizeof(buf),
    "{\"name\":\"HVAC Health Warnings\",\"uniq_id\":\"%s_health\","
    "\"stat_t\":\"%s/health\",\"avty_t\":\"%s/availability\","
    "\"dev\":{\"ids\":[\"%s\"]}}",
    deviceId, baseTopic, baseTopic, deviceId);
  client.publish(cfgTopic, buf, true);
}

// Tolerant "\"key\":number" extraction for Z2M payloads (no JSON lib).
static float jsonNumber(const char* json, const char* key) {
  char pat[32];
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char* p = strstr(json, pat);
  if (!p) return NAN;
  return atof(p + strlen(pat));
}

static void onMessage(char* topic, byte* payload, unsigned int length) {
  static char buf[512];
  if (length >= sizeof(buf)) length = sizeof(buf) - 1;
  memcpy(buf, payload, length);
  buf[length] = '\0';

  char cmdTopic[64];
  snprintf(cmdTopic, sizeof(cmdTopic), "%s/cmd/", baseTopic);
  if (strncmp(topic, cmdTopic, strlen(cmdTopic)) == 0) {
    const char* cmd = topic + strlen(cmdTopic);
    if (!strcmp(cmd, "mode")) {
      SystemMode m = systemMode;
      if (!strcmp(buf, "off")) m = SYS_OFF; else if (!strcmp(buf, "heat")) m = SYS_HEAT;
      else if (!strcmp(buf, "cool")) m = SYS_COOL; else if (!strcmp(buf, "auto")) m = SYS_AUTO;
      else return;
      systemMode = m;
      saveSettings();
      logEvent("mqtt: mode -> %s", buf);
    } else if (!strcmp(cmd, "heat_sp")) {
      float f = atof(buf);
      float cool = lastResolved.coolSetpointF;
      if (cool < f + MIN_SETPOINT_GAP_F) cool = f + MIN_SETPOINT_GAP_F;
      setHold(f, cool, DEFAULT_HOLD_HOURS); // HA setpoint change = temporary hold
    } else if (!strcmp(cmd, "cool_sp")) {
      float f = atof(buf);
      float heat = lastResolved.heatSetpointF;
      if (heat > f - MIN_SETPOINT_GAP_F) heat = f - MIN_SETPOINT_GAP_F;
      setHold(heat, f, DEFAULT_HOLD_HOURS);
    } else if (!strcmp(cmd, "hold_off")) {
      clearHold();
    }
    publishState();
    return;
  }

  // Zigbee2MQTT probe payloads: {"temperature":21.5,"humidity":45.2,"battery":100,...}
  // Z2M default units: Celsius.
  if (netcfg().z2mIndoor[0] && !strcmp(topic, netcfg().z2mIndoor)) {
    float tC = jsonNumber(buf, "temperature");
    if (!isnan(tC)) sensors_injectRemote(celsiusToFahrenheit(tC), jsonNumber(buf, "humidity"));
    float batt = jsonNumber(buf, "battery");
    if (!isnan(batt) && batt < 15) logEvent("HEALTH: indoor probe battery low (%.0f%%)", batt);
  } else if (netcfg().z2mOutdoor[0] && !strcmp(topic, netcfg().z2mOutdoor)) {
    float tC = jsonNumber(buf, "temperature");
    if (!isnan(tC)) sensors_injectOutdoorProbe(celsiusToFahrenheit(tC));
    float batt = jsonNumber(buf, "battery");
    if (!isnan(batt) && batt < 15) logEvent("HEALTH: outdoor probe battery low (%.0f%%)", batt);
  }
}

void mqtt_begin() {
  if (!enabled()) return;
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(deviceId, sizeof(deviceId), "hvacc6-%02x%02x%02x", mac[3], mac[4], mac[5]);
  snprintf(baseTopic, sizeof(baseTopic), "hvac/%s", deviceId);
  client.setServer(netcfg().mqttHost, netcfg().mqttPort);
  client.setBufferSize(1280); // HA discovery payloads exceed the 256B default
  client.setCallback(onMessage);
}

void mqtt_poll() {
  if (!enabled() || !isWifiConnected()) return;

  if (!client.connected()) {
    if (millis() - lastAttemptMs < 15000UL) return;
    lastAttemptMs = millis();
    char avty[64];
    snprintf(avty, sizeof(avty), "%s/availability", baseTopic);
    bool ok = client.connect(deviceId,
                             netcfg().mqttUser[0] ? netcfg().mqttUser : nullptr,
                             netcfg().mqttPass[0] ? netcfg().mqttPass : nullptr,
                             avty, 0, true, "offline");
    if (ok) {
      everConnected = true;
      connectFailLogged = false;
      logEvent("mqtt connected (%s)", deviceId);
      client.publish(avty, "online", true);
      char sub[64];
      snprintf(sub, sizeof(sub), "%s/cmd/#", baseTopic);
      client.subscribe(sub);
      if (netcfg().z2mIndoor[0]) client.subscribe(netcfg().z2mIndoor);
      if (netcfg().z2mOutdoor[0]) client.subscribe(netcfg().z2mOutdoor);
      publishDiscovery();
      publishState();
    } else if (!connectFailLogged) {
      connectFailLogged = true;
      logEvent("mqtt connect failed (rc %d)%s", client.state(), everConnected ? "" : " - check net settings");
    }
    return;
  }

  client.loop();
  if (millis() - lastStateMs > 30000UL) publishState();
}
