#ifndef OTA_H
#define OTA_H

#include <stddef.h>
#include <stdint.h>

// Firmware updates over WiFi, three ways in:
//  - pull: checks OTA_CHANNEL_URL (GitHub release CI publishes from `wip`)
//    every OTA_CHECK_INTERVAL_HOURS; installs on its own in AUTO mode, only
//    while heat/cool are idle and service mode is off.
//  - web upload: /update on the dashboard (basic auth, user "admin" +
//    ota_password).
//  - ArduinoOTA (espota), gated on ota_password.
// Every path turns ALL relays off, flushes learned/health data, and releases
// the task WDT before writing flash (the write blocks the loop).
//
// Trial boot: a new image starts pending-verify (bootloader rollback is
// enabled in the core's bootloader). It is marked valid only after the boot
// self-tests pass and OTA_VERIFY_MINS of uptime (plus a WiFi connection when
// WiFi is configured). A crash or WDT reset before that, a failed self-test,
// or no WiFi by OTA_VERIFY_TIMEOUT_MINS reverts to the previous image, and
// that build is remembered as bad so AUTO mode never retries it. USB flashing
// (firmware.factory.bin at 0x0) remains the recovery path of last resort.

enum OtaMode { OTA_AUTO = 0, OTA_NOTIFY = 1, OTA_OFF = 2 };

void ota_begin();                       // boot: load mode, rollback bookkeeping (after events_begin)
void ota_bootSelfTest(int failures);    // after runTests(): failing tests on a trial image roll back
void ota_poll();                        // every loop()

void ota_requestCheck();
void ota_requestInstall();              // manual: ignores mode, idle wait, and the bad-build memory
void ota_setMode(OtaMode m);            // persisted
OtaMode ota_mode();
const char* ota_modeName(OtaMode m);
bool ota_parseMode(const char* s, OtaMode* out);

const char* ota_statusText();           // one human line: last check / install / verify result
const char* ota_availableVersion();     // "" when none known
bool ota_verifying();                   // trial image not yet confirmed
void ota_printStatus();                 // console

// Web upload (wired to WebServer's upload callback in web.cpp).
bool ota_uploadBegin();
bool ota_uploadWrite(const uint8_t* data, size_t len);
bool ota_uploadEnd();                   // true = image accepted, caller reboots
void ota_uploadAbort();                 // after any failure, once the stream has ended (re-arms the WDT)
const char* ota_uploadError();

// Pure helpers (boot-tested).
bool ota_parseManifest(const char* json, char* sha, size_t shaLen, char* ver, size_t verLen);
bool ota_isAppImage(const uint8_t* buf, size_t len); // ESP32-C6 app, not bootloader/factory image

#endif // OTA_H
