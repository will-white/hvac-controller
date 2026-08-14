#include "events.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <stdarg.h>
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "config.h"
#include "hvac_tests.h"
#include "main.h"
#include "net_time.h"

struct EventEntry {
  unsigned long ms;
  char msg[56];
};

static const int EVENT_CAPACITY = 64;
static EventEntry ring[EVENT_CAPACITY];
static int head = 0;   // next slot to write
static int count = 0;

// -- Flash journal state --
static bool journalReady = false;
// Flash-wear guard: `sim speed` can fire relay transitions far faster than
// real time. The RAM ring always gets everything; the journal caps appends
// per rolling REAL minute and notes what it dropped.
static unsigned long rateWindowStartMs = 0;
static int rateCount = 0;
static unsigned int rateDropped = 0;

const char* resetReasonName(int reason) {
  switch ((esp_reset_reason_t)reason) {
    case ESP_RST_POWERON:    return "power-on";
    case ESP_RST_SW:         return "software";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "int-watchdog";
    case ESP_RST_TASK_WDT:   return "task-watchdog";
    case ESP_RST_WDT:        return "other-watchdog";
    case ESP_RST_DEEPSLEEP:  return "deepsleep-wake";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    default:                 return "unknown";
  }
}

// Open for append, rotating current -> .old past the size cap so one prior
// generation is always retained for post-mortems.
static File openJournal() {
  File f = LittleFS.open(EVENT_JOURNAL_FILE, FILE_APPEND);
  if (f && f.size() > EVENT_JOURNAL_MAX_BYTES) {
    f.close();
    LittleFS.remove(EVENT_JOURNAL_OLD_FILE);
    LittleFS.rename(EVENT_JOURNAL_FILE, EVENT_JOURNAL_OLD_FILE);
    f = LittleFS.open(EVENT_JOURNAL_FILE, FILE_APPEND);
  }
  return f;
}

static void journalAppend(unsigned long uptimeMs, const char* msg) {
  if (!journalReady || g_isTesting) return;

  unsigned long nowReal = millis();
  if (nowReal - rateWindowStartMs >= 60000UL) {
    rateWindowStartMs = nowReal;
    rateCount = 0;
  }
  if (rateCount >= EVENT_JOURNAL_MAX_PER_MIN) { rateDropped++; return; }
  rateCount++;

  File f = openJournal();
  if (!f) return;
  if (rateDropped) {
    f.printf("(journal rate limit: %u event(s) not persisted; see RAM ring)\n", rateDropped);
    rateDropped = 0;
  }
  // Wall time when the clock is synced, uptime always.
  if (isTimeValid()) {
    TimeInfo t = getCurrentTime();
    f.printf("%02d/%02d %02d:%02d [%7lu.%03lus] %s\n",
             t.month, t.day, t.hour, t.minute, uptimeMs / 1000, uptimeMs % 1000, msg);
  } else {
    f.printf("            [%7lu.%03lus] %s\n", uptimeMs / 1000, uptimeMs % 1000, msg);
  }
  f.close();
}

void events_begin() {
  journalReady = true;
  rateWindowStartMs = millis();
  File f = openJournal();
  if (!f) {
    journalReady = false;
    Serial.println("WARNING: event journal unavailable (LittleFS open failed).");
    return;
  }
  f.printf("\n=== BOOT === reset reason: %s\n", resetReasonName((int)esp_reset_reason()));
  f.close();
}

void logEvent(const char* fmt, ...) {
  EventEntry& e = ring[head];
  e.ms = millis();
  va_list args;
  va_start(args, fmt);
  vsnprintf(e.msg, sizeof(e.msg), fmt, args);
  va_end(args);
  head = (head + 1) % EVENT_CAPACITY;
  if (count < EVENT_CAPACITY) count++;
  journalAppend(e.ms, e.msg);
}

void dumpEvents() {
  Serial.printf("--- Event log (%d entries, oldest first) ---\n", count);
  int start = (head - count + EVENT_CAPACITY) % EVENT_CAPACITY;
  for (int i = 0; i < count; i++) {
    const EventEntry& e = ring[(start + i) % EVENT_CAPACITY];
    unsigned long secs = e.ms / 1000;
    Serial.printf("[%5lu.%03lus] %s\n", secs, e.ms % 1000, e.msg);
  }
}

void dumpJournal() {
  if (!journalReady) { Serial.println("Journal unavailable (filesystem not mounted)."); return; }
  File f = LittleFS.open(EVENT_JOURNAL_FILE, FILE_READ);
  if (!f) { Serial.println("Journal is empty."); return; }
  Serial.printf("--- Event journal %s (%u bytes; one older generation in %s) ---\n",
                EVENT_JOURNAL_FILE, (unsigned)f.size(), EVENT_JOURNAL_OLD_FILE);
  // Paced chunks: Serial has a zero TX timeout (drops when the ring is full),
  // and a long dump must keep feeding the task watchdog.
  uint8_t buf[64];
  size_t n;
  while ((n = f.read(buf, sizeof(buf))) > 0) {
    Serial.write(buf, n);
    esp_task_wdt_reset();
    delay(2);
  }
  f.close();
  Serial.println("--- end of journal ---");
}
