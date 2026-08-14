#include "console.h"
#include <Arduino.h>
#include <Wire.h>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include "nvs_flash.h"
#include "config.h"
#include "events.h"
#include "health.h"
#include "hvac_logic.h"
#include "inputs.h"
#include "main.h"
#include "net_time.h"
#include "relays.h"
#include "sensors.h"
#include "setpoints.h"

// -- Virtual clock: monotonic, advances at simSpeed x real time --
static unsigned long virtualMs = 0;
static unsigned long lastRealMs = 0;
static unsigned long simSpeed = 1;

// -- Simulation overrides --
static bool simTempOn = false;     static float simTempF = 0;
static bool simOutdoorOn = false;  static float simOutdoorF = 0;
static bool simRhOn = false;       static float simRhV = 0;
static bool simTimeOn = false;     static TimeInfo simTimeVal = {1, 1, 1, 12, 0};
static unsigned long simTimeAccumMs = 0;

// -- Service (test) mode --
static bool testMode = false;
static unsigned long testLastActionRealMs = 0;          // real millis (safety timers)
static unsigned long relayOnSinceRealMs[RELAY_COUNT] = {0};

static const char* RELAY_NAMES[RELAY_COUNT] = {"heat", "cool", "fan", "damper", "hum", "spare"};

static char lineBuf[96];
static int lineLen = 0;

unsigned long console_virtualMillis() { return virtualMs; }
bool console_testModeActive() { return testMode; }
bool console_simTemp(float& outF)    { if (simTempOn)    outF = simTempF;    return simTempOn; }
bool console_simOutdoor(float& outF) { if (simOutdoorOn) outF = simOutdoorF; return simOutdoorOn; }
bool console_simHumidity(float& out) { if (simRhOn)      out = simRhV;       return simRhOn; }
bool console_simTime(TimeInfo& out)  { if (simTimeOn)    out = simTimeVal;   return simTimeOn; }

static int relayByName(const char* name) {
  for (int i = 0; i < RELAY_COUNT; i++)
    if (strcmp(name, RELAY_NAMES[i]) == 0) return i;
  return -1;
}

static void exitTestMode(const char* why) {
  for (int i = 0; i < RELAY_COUNT; i++) { writeRelay(i, LOW); relayOnSinceRealMs[i] = 0; }
  testMode = false;
  logEvent("service mode exit (%s)", why);
  Serial.printf("Service mode OFF (%s). All relays off; normal control resumes.\n", why);
}

static void printStatus() {
  Serial.println("=== Status ===");
  Serial.printf("Mode: %s%s | fault:%d lockout:%d | service:%d\n",
    systemMode == SYS_OFF ? "OFF" : systemMode == SYS_HEAT ? "HEAT" : systemMode == SYS_COOL ? "COOL" : "AUTO",
    vacationModeActive ? " (VACATION)" : "", systemInFaultState, systemLockedOut, testMode);
  if ((systemInFaultState || systemLockedOut) && g_safetyReason[0])
    Serial.printf("Safety latch reason: %s (persists across reboot until 'reset fault'/'reset lockout')\n", g_safetyReason);
  Serial.printf("Targets: heat-to %.1fF / cool-to %.1fF  [%s]%s\n",
    lastResolved.heatSetpointF, lastResolved.coolSetpointF,
    targetSourceName(lastResolved.source), testMode ? " (stale: control suspended)" : "");
  Serial.printf("Indoor: %.1fF | relays: ", lastIndoorF);
  for (int i = 0; i < RELAY_COUNT; i++)
    Serial.printf("%s:%d ", RELAY_NAMES[i], readRelay(i));
  Serial.println();
  Serial.printf("WiFi: %s | time: %s | sim: temp:%d out:%d rh:%d clock:%d speed:%lux\n",
    isWifiConnected() ? "up" : "down", isTimeValid() ? "synced" : "NOT SYNCED",
    simTempOn, simOutdoorOn, simRhOn, simTimeOn, simSpeed);
  Serial.printf("Uptime: %lus real, %lus virtual | health warnings: %d\n", millis() / 1000, virtualMs / 1000, health_warningCount());
}

static void printHelp() {
  Serial.println(
    "Commands:\n"
    "  status | sensors | inputs | relays | log [file] | i2c | health [reset filter|counters]\n"
    "  remote <tempF> [rh]         (emulate an indoor wireless probe report, honors staleness)\n"
    "  outdoor <tempF>             (emulate the outdoor probe; outranks the weather feed)\n"
    "  mode <off|heat|cool|auto>   vacation <on|off>\n"
    "  hold <heatF> <coolF> [hours] | hold off\n"
    "  test <on|off>               (service mode: suspends control, enables relay cmds)\n"
    "  relay <heat|cool|fan|damper|hum|spare> <on|off> [force]\n"
    "  sim temp <F|nan> | sim outdoor <F|nan> | sim rh <pct> | sim time <dow0-6> <HH:MM> [month]\n"
    "  sim speed <n> | sim off\n"
    "  reset <fault|lockout>       save | reboot | factory confirm");
}

static void handleRelayCmd(const char* name, const char* action, const char* flag) {
  if (!testMode) { Serial.println("Refused: relay control requires 'test on' (service mode)."); return; }
  int r = relayByName(name ? name : "");
  if (r < 0 || !action) { Serial.println("Usage: relay <heat|cool|fan|damper|hum|spare> <on|off> [force]"); return; }
  bool on = strcmp(action, "on") == 0;
  bool force = flag && strcmp(flag, "force") == 0;
  if (on && r == COOLER_RELAY && !coolerMinOffElapsed() && !force) {
    Serial.println("Refused: compressor min-off time not elapsed. Add 'force' to override (know what you're doing).");
    return;
  }
  writeRelay(r, on);
  relayOnSinceRealMs[r] = on ? millis() : 0;
  testLastActionRealMs = millis();
  logEvent("service: relay %s -> %s%s", RELAY_NAMES[r], on ? "ON" : "off", force ? " (forced)" : "");
  Serial.printf("Relay %s -> %s%s\n", RELAY_NAMES[r], on ? "ON" : "off",
    on ? " (auto-off in 5 min)" : "");
}

static void i2cScan() {
  Serial.println("Scanning I2C (UEXT)...");
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  0x%02X%s\n", a,
        a == MODIO2_I2C_ADDRESS ? " (MOD-IO2)" : a == SHT4X_I2C_ADDRESS ? " (SHT4x)" : "");
      found++;
    }
  }
  Serial.printf("%d device(s).\n", found);
}

static void handleLine(char* line) {
  char* cmd = strtok(line, " ");
  if (!cmd) return;
  char* a1 = strtok(nullptr, " ");
  char* a2 = strtok(nullptr, " ");
  char* a3 = strtok(nullptr, " ");

  if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) { printHelp(); }
  else if (!strcmp(cmd, "status")) { printStatus(); }
  else if (!strcmp(cmd, "sensors")) {
    Serial.printf("indoor %.1fF | outdoor %.1fF (%s) | rh %.1f%% | remote %s | SHT4x %s\n",
      readTemperature(), readOutdoorTemperature(), sensors_outdoorSource(), readHumidity(),
      sensors_remoteFresh() ? "fresh" : "none/stale",
      sensors_indoorPresent() ? "present" : "absent");
  }
  else if (!strcmp(cmd, "remote") && a1) {
    // Emulate a wireless probe report end-to-end (goes through staleness logic,
    // unlike `sim temp` which bypasses it).
    sensors_injectRemote(atof(a1), a2 ? atof(a2) : NAN);
    Serial.println("Remote report injected.");
  }
  else if (!strcmp(cmd, "outdoor") && a1) {
    sensors_injectOutdoorProbe(atof(a1));
    Serial.println("Outdoor probe report injected (outranks weather while fresh).");
  }
  else if (!strcmp(cmd, "inputs")) {
    for (int i = 0; i < OPTO_COUNT; i++) {
      Serial.printf("  OPT%d GPIO%-2d %-10s raw:%d active:%d armed:%d\n",
        i + 1, OPTO_PINS[i], optoRoleName(OPTO_ROLES[i]),
        inputs_rawLevel(i), inputs_isActive(i), inputs_isCommissioned(i));
    }
    Serial.println("(armed = has seen a signal at least once; rules stay quiet until then)");
  }
  else if (!strcmp(cmd, "relays")) {
    for (int i = 0; i < RELAY_COUNT; i++) Serial.printf("  %-6s %s\n", RELAY_NAMES[i], readRelay(i) ? "ON" : "off");
  }
  else if (!strcmp(cmd, "relay")) { handleRelayCmd(a1, a2, a3); }
  else if (!strcmp(cmd, "test")) {
    if (a1 && !strcmp(a1, "on") && !testMode) {
      testMode = true;
      testLastActionRealMs = millis();
      logEvent("service mode enter");
      Serial.println("Service mode ON: control suspended, relays under manual command.\n"
                     "Guardrails: per-relay 5-min auto-off, 30-min idle auto-exit, compressor min-off enforced.");
    } else if (a1 && !strcmp(a1, "off") && testMode) { exitTestMode("command"); }
  }
  else if (!strcmp(cmd, "mode") && a1) {
    SystemMode m = systemMode;
    if (!strcmp(a1, "off")) m = SYS_OFF; else if (!strcmp(a1, "heat")) m = SYS_HEAT;
    else if (!strcmp(a1, "cool")) m = SYS_COOL; else if (!strcmp(a1, "auto")) m = SYS_AUTO;
    else { Serial.println("Usage: mode <off|heat|cool|auto>"); return; }
    systemMode = m; saveSettings(); logEvent("mode -> %s", a1);
    Serial.printf("Mode set: %s\n", a1);
  }
  else if (!strcmp(cmd, "vacation") && a1) {
    vacationModeActive = !strcmp(a1, "on"); saveSettings();
    logEvent("vacation -> %s", a1);
  }
  else if (!strcmp(cmd, "hold") && a1) {
    if (!strcmp(a1, "off")) { clearHold(); }
    else if (a2) { setHold(atof(a1), atof(a2), a3 ? strtoul(a3, nullptr, 10) : DEFAULT_HOLD_HOURS); }
    else { Serial.println("Usage: hold <heatF> <coolF> [hours] | hold off"); }
  }
  else if (!strcmp(cmd, "sim") && a1) {
    if (!strcmp(a1, "temp") && a2)    { simTempOn = true;    simTempF = !strcmp(a2, "nan") ? NAN : atof(a2); }
    else if (!strcmp(a1, "outdoor") && a2) { simOutdoorOn = true; simOutdoorF = !strcmp(a2, "nan") ? NAN : atof(a2); }
    else if (!strcmp(a1, "rh") && a2) { simRhOn = true; simRhV = atof(a2); }
    else if (!strcmp(a1, "time") && a2 && a3) {
      simTimeOn = true;
      simTimeVal.dayOfWeek = atoi(a2) % 7;
      simTimeVal.hour = atoi(a3);
      char* colon = strchr(a3, ':');
      simTimeVal.minute = colon ? atoi(colon + 1) : 0;
      char* a4 = strtok(nullptr, " ");
      if (a4) simTimeVal.month = atoi(a4);
    }
    else if (!strcmp(a1, "speed") && a2) { simSpeed = strtoul(a2, nullptr, 10); if (simSpeed < 1) simSpeed = 1; }
    else if (!strcmp(a1, "off")) { simTempOn = simOutdoorOn = simRhOn = simTimeOn = false; simSpeed = 1; }
    else { Serial.println("Usage: sim temp|outdoor <F|nan> | sim rh <pct> | sim time <dow> <HH:MM> [month] | sim speed <n> | sim off"); return; }
    logEvent("sim: %s %s", a1, a2 ? a2 : "");
    Serial.println("ok");
  }
  else if (!strcmp(cmd, "reset") && a1) {
    if (!strcmp(a1, "fault")) { systemInFaultState = false; saveSafetyState(nullptr); logEvent("fault acknowledged via console"); Serial.println("Fault cleared."); }
    else if (!strcmp(a1, "lockout")) { resetLockout(); saveSafetyState(nullptr); logEvent("lockout acknowledged via console"); Serial.println("Lockout cleared."); }
  }
  else if (!strcmp(cmd, "health")) {
    if (a1 && !strcmp(a1, "reset") && a2 && !strcmp(a2, "filter")) { health_resetFilter(); Serial.println("Filter runtime reset."); }
    else if (a1 && !strcmp(a1, "reset") && a2 && !strcmp(a2, "counters")) { health_resetCounters(); Serial.println("All health counters reset."); }
    else printHealthReport();
  }
  else if (!strcmp(cmd, "log")) {
    if (a1 && !strcmp(a1, "file")) dumpJournal(); // flash journal (survives reboots)
    else dumpEvents();                            // RAM ring (this boot only)
  }
  else if (!strcmp(cmd, "i2c")) { i2cScan(); }
  else if (!strcmp(cmd, "save")) { saveSettings(); savePerformance(); health_flushIfDirty(); Serial.println("Saved settings + learning + health data."); }
  else if (!strcmp(cmd, "reboot")) { if (performanceDirty) savePerformance(); health_flushIfDirty(); Serial.println("Rebooting..."); Serial.flush(); esp_restart(); }
  else if (!strcmp(cmd, "factory")) {
    if (a1 && !strcmp(a1, "confirm")) { nvs_flash_erase(); Serial.println("NVS erased. Rebooting..."); Serial.flush(); esp_restart(); }
    else Serial.println("This erases ALL settings and learned data. Type 'factory confirm' to proceed.");
  }
  else { Serial.printf("Unknown command '%s' - try 'help'.\n", cmd); }
}

void console_begin() {
  virtualMs = millis();
  lastRealMs = millis();
  Serial.println("Console ready - type 'help'.");
}

void console_poll() {
  // Advance the virtual clock (drives currentTime() and sim schedule time).
  unsigned long nowReal = millis();
  unsigned long delta = nowReal - lastRealMs;
  lastRealMs = nowReal;
  virtualMs += delta * simSpeed;
  if (simTimeOn && simSpeed > 1) {
    simTimeAccumMs += delta * simSpeed;
    while (simTimeAccumMs >= 60000UL) {
      simTimeAccumMs -= 60000UL;
      if (++simTimeVal.minute >= 60) {
        simTimeVal.minute = 0;
        if (++simTimeVal.hour >= 24) { simTimeVal.hour = 0; simTimeVal.dayOfWeek = (simTimeVal.dayOfWeek + 1) % 7; }
      }
    }
  }

  // Service-mode safety timers run on REAL time, always.
  if (testMode) {
    for (int i = 0; i < RELAY_COUNT; i++) {
      if (relayOnSinceRealMs[i] && nowReal - relayOnSinceRealMs[i] > TEST_MODE_RELAY_TIMEOUT_MS) {
        writeRelay(i, LOW);
        relayOnSinceRealMs[i] = 0;
        logEvent("service: relay %s auto-off (timeout)", RELAY_NAMES[i]);
        Serial.printf("Relay %s auto-off (5 min service timeout).\n", RELAY_NAMES[i]);
      }
    }
    if (nowReal - testLastActionRealMs > TEST_MODE_IDLE_TIMEOUT_MS) exitTestMode("idle timeout");
  }

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      lineBuf[lineLen] = '\0';
      lineLen = 0;
      if (lineBuf[0]) handleLine(lineBuf);
    } else if (lineLen < (int)sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}
