#include "web.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <cmath>
#include "config.h"
#include "console.h"
#include "events.h"
#include "health.h"
#include "hvac_logic.h"
#include "main.h"
#include "fw_version.h"
#include "net_time.h"
#include "netconfig.h"
#include "ota.h"
#include "relays.h"
#include "sensors.h"
#include "setpoints.h"

static WebServer server(80);
static bool started = false;

static const char PAGE[] PROGMEM = R"html(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>HVAC Controller</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;background:#f4f5f7;color:#1a1d21}
header{background:#1f3a5f;color:#fff;padding:.8rem 1rem;font-size:1.1rem;font-weight:600}
main{max-width:640px;margin:0 auto;padding:1rem}
.card{background:#fff;border-radius:8px;padding:1rem;margin-bottom:1rem;box-shadow:0 1px 3px rgba(0,0,0,.08)}
.big{font-size:2.6rem;font-weight:700}
.muted{color:#667;font-size:.85rem}
.row{display:flex;gap:.6rem;flex-wrap:wrap;align-items:center}
button{border:0;border-radius:6px;padding:.5rem .9rem;background:#e2e6ea;cursor:pointer;font-size:.95rem}
button.active{background:#1f3a5f;color:#fff}
button.warn{background:#c0392b;color:#fff}
input{width:4.5rem;padding:.4rem;border:1px solid #ccd;border-radius:6px}
.pill{display:inline-block;padding:.15rem .55rem;border-radius:999px;font-size:.8rem;background:#e2e6ea;margin-right:.3rem}
.pill.on{background:#2e7d32;color:#fff}
</style></head><body>
<header>HVAC Controller</header><main>
<div class="card"><div class="big"><span id="temp">--</span>&deg;F</div>
<div class="muted">humidity <span id="rh">--</span>% &middot; outdoor <span id="outdoor">--</span>&deg;F (<span id="osrc">-</span>)</div>
<div style="margin-top:.5rem">heat-to <b><span id="hsp">--</span>&deg;</b> / cool-to <b><span id="csp">--</span>&deg;</b>
<span class="muted">[<span id="src">-</span>]</span></div>
<div style="margin-top:.5rem" id="relays"></div>
<div class="muted" style="margin-top:.5rem">action: <span id="action">-</span> &middot; health warnings: <span id="health">-</span> &middot; <span id="flags"></span></div></div>
<div class="card"><b>Mode</b><div class="row" style="margin-top:.5rem">
<button onclick="setMode('off')" id="m_off">Off</button>
<button onclick="setMode('heat')" id="m_heat">Heat</button>
<button onclick="setMode('cool')" id="m_cool">Cool</button>
<button onclick="setMode('auto')" id="m_auto">Auto</button></div></div>
<div class="card"><b>Temporary hold</b><div class="row" style="margin-top:.5rem">
heat <input id="hold_h" type="number" step="0.5" value="70">
cool <input id="hold_c" type="number" step="0.5" value="75">
hours <input id="hold_t" type="number" value="2">
<button class="active" onclick="setHold()">Hold</button>
<button onclick="post('/api/holdoff')">Release</button></div></div>
<div class="card"><b>Safety</b><div class="row" style="margin-top:.5rem">
<button class="warn" onclick="post('/api/reset?what=fault')">Reset fault</button>
<button class="warn" onclick="post('/api/reset?what=lockout')">Reset lockout</button></div>
<div class="muted" style="margin-top:.4rem">Only after fixing the cause. Service mode & simulation live on the USB console.</div></div>
<div class="card"><b>Firmware</b> <span class="muted" id="fw">-</span>
<div style="margin-top:.4rem" id="upd">-</div>
<div class="row" style="margin-top:.5rem">automatic updates:
<button onclick="post('/api/update?action=mode&m=auto')" id="u_auto">Auto</button>
<button onclick="post('/api/update?action=mode&m=notify')" id="u_notify">Notify</button>
<button onclick="post('/api/update?action=mode&m=off')" id="u_off">Off</button></div>
<div class="row" style="margin-top:.5rem">
<button onclick="post('/api/update?action=check')">Check now</button>
<button class="active" id="u_install" onclick="if(window.confirm('Install now? Heating/cooling stop for about a minute.'))post('/api/update?action=install')">Install update</button>
<a href="/update" class="muted">upload a file</a></div>
<div class="muted" style="margin-top:.4rem">A new build runs on trial and rolls back on its own if it fails. USB flashing always recovers.</div></div>
<script>
async function post(u){await fetch(u,{method:'POST'});refresh()}
function setMode(m){post('/api/mode?m='+m)}
function setHold(){post('/api/hold?heat='+hold_h.value+'&cool='+hold_c.value+'&hours='+hold_t.value)}
async function refresh(){try{
let s=await (await fetch('/api/status')).json();
temp.textContent=s.temp;rh.textContent=s.rh;outdoor.textContent=s.outdoor;osrc.textContent=s.outdoorSrc;
hsp.textContent=s.heatSp;csp.textContent=s.coolSp;src.textContent=s.src;action.textContent=s.action;
health.textContent=s.health;
relays.innerHTML=s.relays.map(r=>'<span class="pill'+(r.on?' on':'')+'">'+r.n+'</span>').join('');
flags.textContent=(s.fault?'FAULT ':'')+(s.lockout?'LOCKOUT ':'')+(s.service?'SERVICE-MODE ':'')+(s.why?'('+s.why+')':'');
for(const m of ['off','heat','cool','auto'])document.getElementById('m_'+m).className=(s.mode==m)?'active':'';
fw.textContent=s.fw;upd.textContent=s.upd;u_install.hidden=!s.updAvail;u_install.textContent='Install '+s.updAvail;
for(const m of ['auto','notify','off'])document.getElementById('u_'+m).className=(s.updMode==m)?'active':'';
}catch(e){}}
refresh();setInterval(refresh,5000);
</script></main></body></html>)html";

static const char* modeName(SystemMode m) {
  switch (m) { case SYS_OFF: return "off"; case SYS_HEAT: return "heat";
               case SYS_COOL: return "cool"; default: return "auto"; }
}

static void handleStatus() {
  static const char* names[RELAY_COUNT] = {"heat", "cool", "fan", "damper", "hum", "spare"};
  const char* action = "idle";
  if (systemInFaultState || systemMode == SYS_OFF) action = "off";
  else if (readRelay(HEATER_RELAY)) action = "heating";
  else if (readRelay(COOLER_RELAY)) action = "cooling";
  else if (readRelay(FAN_RELAY)) action = "fan";

  char relays[160] = "";
  int off = 0;
  for (int i = 0; i < RELAY_COUNT; i++) {
    off += snprintf(relays + off, sizeof(relays) - off, "%s{\"n\":\"%s\",\"on\":%d}",
                    i ? "," : "", names[i], readRelay(i) ? 1 : 0);
  }

  char buf[960];
  float rh = readHumidity();
  float outdoor = readOutdoorTemperature();
  snprintf(buf, sizeof(buf),
    "{\"temp\":%.1f,\"rh\":%.0f,\"outdoor\":%.1f,\"outdoorSrc\":\"%s\","
    "\"heatSp\":%.1f,\"coolSp\":%.1f,\"src\":\"%s\",\"mode\":\"%s\",\"action\":\"%s\","
    "\"relays\":[%s],\"fault\":%d,\"lockout\":%d,\"service\":%d,\"health\":%d,"
    "\"why\":\"%s\",\"rssi\":%d,\"uptime\":%lu,"
    "\"fw\":\"%s\",\"updMode\":\"%s\",\"upd\":\"%s\",\"updAvail\":\"%s\",\"verifying\":%d}",
    lastIndoorF, isnan(rh) ? -1.0f : rh, isnan(outdoor) ? -99.0f : outdoor,
    sensors_outdoorSource(), lastResolved.heatSetpointF, lastResolved.coolSetpointF,
    targetSourceName(lastResolved.source), modeName(systemMode), action, relays,
    systemInFaultState, systemLockedOut, console_testModeActive() ? 1 : 0, health_warningCount(),
    // reason strings are compile-time constants (no quotes/backslashes to escape)
    (systemInFaultState || systemLockedOut) ? g_safetyReason : "",
    (int)WiFi.RSSI(), millis() / 1000UL,
    // update strings are built from versions, numbers and esp_err names only
    FW_VERSION, ota_modeName(ota_mode()), ota_statusText(), ota_availableVersion(),
    ota_verifying() ? 1 : 0);
  server.send(200, "application/json", buf);
}

static void handleMode() {
  String m = server.arg("m");
  SystemMode mode = systemMode;
  if (m == "off") mode = SYS_OFF; else if (m == "heat") mode = SYS_HEAT;
  else if (m == "cool") mode = SYS_COOL; else if (m == "auto") mode = SYS_AUTO;
  else { server.send(400, "text/plain", "bad mode"); return; }
  systemMode = mode;
  saveSettings();
  logEvent("web: mode -> %s", m.c_str());
  server.send(200, "text/plain", "ok");
}

static void handleHold() {
  setHold(server.arg("heat").toFloat(), server.arg("cool").toFloat(),
          server.arg("hours").length() ? server.arg("hours").toInt() : DEFAULT_HOLD_HOURS);
  server.send(200, "text/plain", "ok");
}

static void handleReset() {
  String what = server.arg("what");
  if (what == "fault") { systemInFaultState = false; saveSafetyState(nullptr); logEvent("fault acknowledged via web"); }
  else if (what == "lockout") { resetLockout(); saveSafetyState(nullptr); logEvent("lockout acknowledged via web"); }
  else { server.send(400, "text/plain", "bad target"); return; }
  server.send(200, "text/plain", "ok");
}

static void handleUpdateApi() {
  String action = server.arg("action");
  if (action == "check") ota_requestCheck();
  else if (action == "install") ota_requestInstall();
  else if (action == "mode") {
    OtaMode m;
    if (!ota_parseMode(server.arg("m").c_str(), &m)) { server.send(400, "text/plain", "bad mode"); return; }
    ota_setMode(m);
  } else { server.send(400, "text/plain", "bad action"); return; }
  server.send(200, "text/plain", "ok");
}

// -- /update: upload firmware.bin from a browser --
static const char UPLOAD_PAGE[] PROGMEM = R"html(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Upload firmware</title>
<style>body{font-family:system-ui,sans-serif;max-width:560px;margin:2rem auto;padding:0 1rem;color:#1a1d21}
button{border:0;border-radius:6px;padding:.5rem .9rem;background:#1f3a5f;color:#fff;font-size:.95rem}
.muted{color:#667;font-size:.9rem}</style></head><body>
<h2>Upload firmware</h2>
<p class="muted">Choose <b>firmware.bin</b> (not firmware.factory.bin). Heating and cooling stop during the upload; the
thermostat reboots, runs the new build on trial, and rolls back on its own if it fails.</p>
<form method="POST" action="/update" enctype="multipart/form-data">
<p><input type="file" name="firmware" accept=".bin" required></p>
<button>Upload and install</button></form>
<p><a href="/">Back to dashboard</a></p></body></html>)html";

static bool uploadAuthorized = false;
static bool uploadStarted = false; // flash guard engaged: must end in reboot or ota_uploadAbort()
static bool uploadOk = false;

static bool requireOtaAuth() {
  const char* pw = netcfg().otaPassword;
  if (!pw[0]) {
    server.send(403, "text/plain", "Uploads are disabled: set an OTA password first (console: net set ota_password <pw>).");
    return false;
  }
  if (!server.authenticate("admin", pw)) { server.requestAuthentication(); return false; }
  return true;
}

static void handleUploadChunk() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    const char* pw = netcfg().otaPassword;
    uploadAuthorized = pw[0] && server.authenticate("admin", pw);
    uploadStarted = uploadAuthorized;
    uploadOk = uploadAuthorized && ota_uploadBegin();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadOk) uploadOk = ota_uploadWrite(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadOk) uploadOk = ota_uploadEnd();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    // Connection dropped: the completion handler may never run.
    if (uploadStarted) ota_uploadAbort();
    uploadStarted = uploadOk = false;
  }
}

static void handleUploadDone() {
  if (!uploadAuthorized) { requireOtaAuth(); return; }
  if (!uploadOk) {
    if (uploadStarted) ota_uploadAbort();
    uploadStarted = false;
    String msg = String("Update failed: ") + ota_uploadError();
    server.send(500, "text/plain", msg);
    return;
  }
  server.send(200, "text/html",
    "<meta http-equiv=refresh content='40;url=/'>Installed. Rebooting into the new build; "
    "this page returns to the dashboard in 40 s.");
  delay(500); // let the response leave before the reboot
  ESP.restart();
}

void web_poll() {
  if (!started) {
    if (!isWifiConnected()) return;
    server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", PAGE); });
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/mode", HTTP_POST, handleMode);
    server.on("/api/hold", HTTP_POST, handleHold);
    server.on("/api/holdoff", HTTP_POST, []() { clearHold(); server.send(200, "text/plain", "ok"); });
    server.on("/api/reset", HTTP_POST, handleReset);
    server.on("/api/update", HTTP_POST, handleUpdateApi);
    server.on("/update", HTTP_GET, []() { if (requireOtaAuth()) server.send_P(200, "text/html", UPLOAD_PAGE); });
    server.on("/update", HTTP_POST, handleUploadDone, handleUploadChunk);
    server.begin();
    started = true;
    logEvent("web dashboard up at http://%s/", WiFi.localIP().toString().c_str());
  }
  server.handleClient();
}
