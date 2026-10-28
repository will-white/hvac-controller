#include "ota.h"
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <cstring>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include "config.h"
#include "console.h"
#include "events.h"
#include "fw_version.h"
#include "health.h"
#include "main.h"
#include "net_time.h"
#include "netconfig.h"
#include "relays.h"

// The core's initArduino() marks a pending-verify image valid before setup()
// unless this returns true. We decide instead: ota_bootSelfTest() + ota_poll().
extern "C" bool verifyRollbackLater() { return true; }

static OtaMode mode = OTA_AUTO;
static char badSha[48] = "";          // build that rolled back; AUTO never retries it
static bool pendingVerify = false;
static unsigned long verifyStartMs = 0;
static bool wifiSeen = false;
static unsigned long wifiUpSinceMs = 0;
static bool flashing = false;
static char statusText[96] = "no update check yet";

// Channel check runs in its own task: a TLS handshake plus GitHub's redirect
// can outlast the WDT window, and control must keep ticking meanwhile.
static volatile bool checkRunning = false;
static volatile bool checkDone = false;
static int chkHttp = 0;
static char chkSha[48], chkVer[32];
static bool everChecked = false;
static unsigned long lastCheckMs = 0;
static unsigned long checkIntervalMs = OTA_CHECK_INTERVAL_HOURS * 3600000UL;
static bool checkRequested = false;
static bool installRequested = false;
static char availSha[48] = "";
static char availVer[32] = "";

// ---------------------------------------------------------------- NVS ------
static void nvsGet(const char* key, char* out, size_t len) {
  out[0] = '\0';
  nvs_handle_t h;
  if (nvs_open("ota", NVS_READONLY, &h) != ESP_OK) return;
  if (nvs_get_str(h, key, out, &len) != ESP_OK) out[0] = '\0';
  nvs_close(h);
}

static void nvsSet(const char* key, const char* val) {
  nvs_handle_t h;
  if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;
  if (val && val[0]) nvs_set_str(h, key, val); else nvs_erase_key(h, key);
  nvs_commit(h);
  nvs_close(h);
}

// Remember which build went into which slot, so the next boot can tell a
// successful update from a bootloader rollback.
static void recordPending(const char* sha) {
  const esp_partition_t* p = esp_ota_get_boot_partition();
  nvsSet("pend", sha);
  nvsSet("pend_part", p ? p->label : "");
}

static void clearPending() {
  nvsSet("pend", nullptr);
  nvsSet("pend_part", nullptr);
}

// ------------------------------------------------- shared flash guard ------
static void prepareForFlash(const char* how) {
  if (flashing) return;
  // Never reflash with equipment energized, and don't lose pending data.
  for (int i = 0; i < RELAY_COUNT; i++) writeRelay(i, LOW);
  if (performanceDirty) savePerformance();
  health_flushIfDirty();
  logEvent("update via %s: relays off", how);
  // Flash writes stall the loop far past the WDT window.
  esp_task_wdt_delete(NULL);
  flashing = true;
}

static void flashAborted() {
  if (!flashing) return;
  esp_task_wdt_add(NULL); // resume normal watchdog coverage
  flashing = false;
}

static void rollBackNow(const char* why) {
  logEvent("rollback: %s", why);
  for (int i = 0; i < RELAY_COUNT; i++) writeRelay(i, LOW);
  if (performanceDirty) savePerformance();
  health_flushIfDirty();
  Serial.flush();
  esp_ota_mark_app_invalid_rollback_and_reboot();
  // Only returns when there is no valid image to go back to: keep this one.
  logEvent("rollback impossible, keeping %s", FW_VERSION);
  esp_ota_mark_app_valid_cancel_rollback();
  pendingVerify = false;
  clearPending();
}

// ------------------------------------------------------------ boot ---------
void ota_begin() {
  nvs_handle_t h;
  if (nvs_open("ota", NVS_READONLY, &h) == ESP_OK) {
    uint8_t m;
    if (nvs_get_u8(h, "mode", &m) == ESP_OK && m <= OTA_OFF) mode = (OtaMode)m;
    nvs_close(h);
  }
  nvsGet("bad", badSha, sizeof(badSha));

  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  pendingVerify = running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
                  st == ESP_OTA_IMG_PENDING_VERIFY;

  char pend[48], part[17];
  nvsGet("pend", pend, sizeof(pend));
  nvsGet("pend_part", part, sizeof(part));
  if (pend[0] && running && strcmp(part, running->label) != 0) {
    // We wrote `part` but booted the other slot: the new build crashed, failed
    // its self-test, or never reached WiFi, and the bootloader reverted.
    strlcpy(badSha, pend, sizeof(badSha));
    nvsSet("bad", badSha);
    clearPending();
    snprintf(statusText, sizeof(statusText), "update %.12s failed and was rolled back", pend);
    logEvent("update %.12s rolled back", pend);
  }

  if (pendingVerify) {
    verifyStartMs = millis();
    snprintf(statusText, sizeof(statusText), "verifying new firmware %s", FW_VERSION);
    logEvent("trial boot of %s", FW_VERSION);
  }
  Serial.printf("Firmware %s (%s)%s\n", FW_VERSION, running ? running->label : "?",
                pendingVerify ? " - trial boot, verifying" : "");
}

void ota_bootSelfTest(int failures) {
  if (!pendingVerify || failures == 0) return;
  char why[48];
  snprintf(why, sizeof(why), "%d self-test(s) failed", failures);
  rollBackNow(why);
}

static void verifyTick() {
  if (!pendingVerify) return;
  unsigned long up = millis() - verifyStartMs;
  bool wifiOk = netcfg().wifiSsid[0] == '\0' || wifiSeen;
  if (up >= OTA_VERIFY_MINS * 60000UL && wifiOk) {
    esp_ota_mark_app_valid_cancel_rollback();
    pendingVerify = false;
    clearPending();
    snprintf(statusText, sizeof(statusText), "firmware %s verified", FW_VERSION);
    logEvent("firmware %s verified", FW_VERSION);
  } else if (up >= OTA_VERIFY_TIMEOUT_MINS * 60000UL) {
    rollBackNow("no WiFi on trial boot");
  }
}

// ------------------------------------------------------ ArduinoOTA ---------
static void arduinoOtaTick(bool wifi) {
  static bool started = false;
  if (netcfg().otaPassword[0] == '\0' || !wifi) return;
  if (!started) {
    ArduinoOTA.setHostname("hvac-controller");
    ArduinoOTA.setPassword(netcfg().otaPassword);
    ArduinoOTA.onStart([]() { prepareForFlash("espota"); });
    ArduinoOTA.onEnd([]() { recordPending("espota"); });
    ArduinoOTA.onError([](ota_error_t e) {
      logEvent("espota error %d", (int)e);
      flashAborted();
    });
    ArduinoOTA.begin();
    started = true;
    logEvent("espota ready (hvac-controller)");
  }
  ArduinoOTA.handle();
}

// ------------------------------------------------------ pull updates -------
struct HttpBuf { char* p; size_t cap; size_t len; };

static esp_err_t onHttpEvent(esp_http_client_event_t* e) {
  HttpBuf* b = (HttpBuf*)e->user_data;
  if (!b) return ESP_OK;
  // A redirect hop sends a new request: drop the previous hop's body.
  if (e->event_id == HTTP_EVENT_HEADERS_SENT) { b->len = 0; b->p[0] = '\0'; }
  if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0) {
    size_t room = b->cap - 1 - b->len;
    size_t n = (size_t)e->data_len < room ? (size_t)e->data_len : room;
    memcpy(b->p + b->len, e->data, n);
    b->len += n;
    b->p[b->len] = '\0';
  }
  return ESP_OK;
}

static void httpConfig(esp_http_client_config_t& c, const char* url, int timeoutMs) {
  memset(&c, 0, sizeof(c));
  c.url = url;
  c.crt_bundle_attach = esp_crt_bundle_attach;
  c.timeout_ms = timeoutMs;
  // GitHub answers with a 302 to a signed CDN URL several hundred bytes long.
  c.buffer_size = 2048;
  c.buffer_size_tx = 2048;
  c.max_redirection_count = 5;
}

static void checkTask(void*) {
  char url[160];
  snprintf(url, sizeof(url), "%smanifest.json", OTA_CHANNEL_URL);
  static char body[512];
  HttpBuf buf = {body, sizeof(body), 0};
  body[0] = '\0';
  esp_http_client_config_t c;
  httpConfig(c, url, 8000);
  c.event_handler = onHttpEvent;
  c.user_data = &buf;
  chkHttp = -1;
  chkSha[0] = chkVer[0] = '\0';
  esp_http_client_handle_t h = esp_http_client_init(&c);
  if (h) {
    if (esp_http_client_perform(h) == ESP_OK) chkHttp = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
  }
  if (chkHttp == 200 && !ota_parseManifest(body, chkSha, sizeof(chkSha), chkVer, sizeof(chkVer)))
    chkHttp = -2;
  checkDone = true;
  checkRunning = false;
  vTaskDelete(NULL);
}

static void startCheck() {
  if (esp_get_free_heap_size() < 60000) {
    snprintf(statusText, sizeof(statusText), "check skipped: low memory");
    lastCheckMs = millis();
    everChecked = true;
    return;
  }
  checkRunning = true;
  checkDone = false;
  if (xTaskCreate(checkTask, "ota_check", 10240, nullptr, 1, nullptr) != pdPASS) {
    checkRunning = false;
    snprintf(statusText, sizeof(statusText), "check failed: no task");
  }
}

static void consumeCheck() {
  checkDone = false;
  everChecked = true;
  lastCheckMs = millis();
  checkIntervalMs = OTA_CHECK_INTERVAL_HOURS * 3600000UL;
  if (chkHttp != 200) {
    checkIntervalMs = OTA_RETRY_MINS * 60000UL;
    if (chkHttp == -2) snprintf(statusText, sizeof(statusText), "check failed: bad manifest");
    else snprintf(statusText, sizeof(statusText), "check failed (HTTP %d)", chkHttp);
    logEvent("update check failed (%d)", chkHttp);
    installRequested = false;
    return;
  }
  if (strcmp(chkSha, FW_GIT_SHA) == 0) {
    availSha[0] = availVer[0] = '\0';
    installRequested = false;
    snprintf(statusText, sizeof(statusText), "up to date (%s)", FW_VERSION);
    return;
  }
  bool newlyFound = strcmp(availSha, chkSha) != 0;
  strlcpy(availSha, chkSha, sizeof(availSha));
  strlcpy(availVer, chkVer, sizeof(availVer));
  if (strcmp(availSha, badSha) == 0)
    snprintf(statusText, sizeof(statusText), "%s rolled back before; install manually to retry", availVer);
  else
    snprintf(statusText, sizeof(statusText), "update available: %s", availVer);
  if (newlyFound) logEvent("update available: %s", availVer);
}

static void install() {
  char url[160];
  snprintf(url, sizeof(url), "%sfirmware.bin", OTA_CHANNEL_URL);
  char sha[48], ver[32];
  strlcpy(sha, availSha, sizeof(sha));
  strlcpy(ver, availVer, sizeof(ver));
  Serial.printf("Installing %s from %s\n", ver, url);
  prepareForFlash("pull");

  esp_http_client_config_t hc;
  httpConfig(hc, url, 15000);
  esp_https_ota_config_t oc = {};
  oc.http_config = &hc;
  esp_https_ota_handle_t h = nullptr;
  esp_err_t e = esp_https_ota_begin(&oc, &h);
  unsigned long t0 = millis();
  while (e == ESP_OK) {
    e = esp_https_ota_perform(h);
    if (e != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
    if (millis() - t0 > OTA_INSTALL_TIMEOUT_MS) { e = ESP_ERR_TIMEOUT; break; }
    e = ESP_OK;
  }
  if (e == ESP_OK && h && esp_https_ota_is_complete_data_received(h)) {
    e = esp_https_ota_finish(h); // validates the image, then sets the boot slot
    h = nullptr;
    if (e == ESP_OK) {
      recordPending(sha);
      logEvent("installed %s, rebooting", ver);
      Serial.flush();
      delay(200);
      esp_restart();
    }
  }
  if (h) esp_https_ota_abort(h);
  // Forget the offer and look again later: retrying at once would loop
  // downloads (each one cutting the relays) while the network is bad.
  availSha[0] = availVer[0] = '\0';
  lastCheckMs = millis();
  checkIntervalMs = OTA_RETRY_MINS * 60000UL;
  snprintf(statusText, sizeof(statusText), "install of %s failed: %s", ver, esp_err_to_name(e));
  logEvent("install failed: %s", esp_err_to_name(e));
  flashAborted();
}

static void pullTick(bool wifi) {
  if (!wifi || pendingVerify || checkRunning || flashing) return;
  unsigned long now = millis();
  bool due = checkRequested ||
             (mode != OTA_OFF && wifiUpSinceMs && now - wifiUpSinceMs > OTA_FIRST_CHECK_DELAY_MS &&
              (!everChecked || now - lastCheckMs > checkIntervalMs));
  if (due) {
    checkRequested = false;
    startCheck();
    return;
  }
  if (!availSha[0]) return;
  bool manual = installRequested;
  if (!manual) {
    if (mode != OTA_AUTO || strcmp(availSha, badSha) == 0) return;
    // Wait for an idle moment: never cut a running heat or cool cycle.
    if (readRelay(HEATER_RELAY) || readRelay(COOLER_RELAY) || console_testModeActive()) return;
  }
  installRequested = false;
  install();
}

void ota_poll() {
  bool wifi = isWifiConnected();
  if (wifi) {
    wifiSeen = true;
    if (!wifiUpSinceMs) wifiUpSinceMs = millis() | 1;
  } else {
    wifiUpSinceMs = 0;
  }
  verifyTick();
  arduinoOtaTick(wifi);
  if (checkDone) consumeCheck();
  pullTick(wifi);
}

// --------------------------------------------------------- controls --------
void ota_requestCheck() { checkRequested = true; }

void ota_requestInstall() {
  installRequested = true;
  if (!availSha[0]) checkRequested = true; // find out what to install first
}

void ota_setMode(OtaMode m) {
  mode = m;
  nvs_handle_t h;
  if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, "mode", (uint8_t)m);
    nvs_commit(h);
    nvs_close(h);
  }
  logEvent("update mode -> %s", ota_modeName(m));
}

OtaMode ota_mode() { return mode; }

const char* ota_statusText() {
  static char line[128];
  if (pendingVerify) {
    unsigned long left = OTA_VERIFY_MINS * 60UL - min((millis() - verifyStartMs) / 1000UL, OTA_VERIFY_MINS * 60UL);
    snprintf(line, sizeof(line), "verifying %s (%lus left%s)", FW_VERSION, left,
             wifiSeen || !netcfg().wifiSsid[0] ? "" : ", waiting for WiFi");
    return line;
  }
  if (checkRunning) return "checking for updates...";
  if (installRequested) return "install requested...";
  return statusText;
}

const char* ota_availableVersion() { return availVer; }
bool ota_verifying() { return pendingVerify; }

void ota_printStatus() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  Serial.printf("Firmware %s (%s, slot %s)\n", FW_VERSION, FW_GIT_SHA, running ? running->label : "?");
  Serial.printf("Update mode: %s | %s\n", ota_modeName(mode), ota_statusText());
  if (everChecked) Serial.printf("Last check %lus ago\n", (millis() - lastCheckMs) / 1000UL);
  if (badSha[0]) Serial.printf("Rolled-back build (skipped by auto): %.12s\n", badSha);
  Serial.printf("Channel: %s\n", OTA_CHANNEL_URL);
}

// ------------------------------------------------------- web upload --------
// Failures inside the upload only record the error: the browser keeps
// streaming the rest of the file through the same blocking handleClient()
// call, so the WDT must stay released until ota_uploadAbort() runs, from the
// request's completion handler or on a dropped connection.
static char uploadErr[64] = "";
static bool uploadChecked = false;

bool ota_uploadBegin() {
  uploadErr[0] = '\0';
  uploadChecked = false;
  prepareForFlash("web upload");
  if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    snprintf(uploadErr, sizeof(uploadErr), "begin: %s", Update.errorString());
    return false;
  }
  return true;
}

bool ota_uploadWrite(const uint8_t* data, size_t len) {
  if (uploadErr[0]) return false;
  if (!uploadChecked) {
    uploadChecked = true;
    if (!ota_isAppImage(data, len)) {
      snprintf(uploadErr, sizeof(uploadErr), "not an ESP32-C6 app image (use firmware.bin)");
      Update.abort();
      return false;
    }
  }
  if (Update.write((uint8_t*)data, len) != len) {
    snprintf(uploadErr, sizeof(uploadErr), "write: %s", Update.errorString());
    Update.abort();
    return false;
  }
  return true;
}

bool ota_uploadEnd() {
  if (uploadErr[0]) return false;
  if (!Update.end(true)) {
    snprintf(uploadErr, sizeof(uploadErr), "end: %s", Update.errorString());
    return false;
  }
  recordPending("upload");
  logEvent("web upload installed, rebooting");
  return true;
}

void ota_uploadAbort() {
  if (Update.isRunning()) Update.abort();
  if (!uploadErr[0]) snprintf(uploadErr, sizeof(uploadErr), "upload aborted");
  logEvent("web upload failed: %.30s", uploadErr);
  flashAborted();
}

const char* ota_uploadError() { return uploadErr[0] ? uploadErr : "unknown error"; }
