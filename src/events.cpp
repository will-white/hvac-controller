#include "events.h"
#include <Arduino.h>
#include <stdarg.h>

struct EventEntry {
  unsigned long ms;
  char msg[56];
};

static const int EVENT_CAPACITY = 64;
static EventEntry ring[EVENT_CAPACITY];
static int head = 0;   // next slot to write
static int count = 0;

void logEvent(const char* fmt, ...) {
  EventEntry& e = ring[head];
  e.ms = millis();
  va_list args;
  va_start(args, fmt);
  vsnprintf(e.msg, sizeof(e.msg), fmt, args);
  va_end(args);
  head = (head + 1) % EVENT_CAPACITY;
  if (count < EVENT_CAPACITY) count++;
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
