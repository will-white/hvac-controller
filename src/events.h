#ifndef EVENTS_H
#define EVENTS_H

// In-RAM event ring buffer: relay transitions, faults, mode changes, service
// actions. Dumped by the console `log` command. Once events_begin() runs (needs
// LittleFS mounted), every event is also appended to a rotating flash journal
// that survives reboots — dumped by `log file`.
void logEvent(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void dumpEvents();

void events_begin();   // enable the flash journal; writes a boot marker w/ reset reason
void dumpJournal();    // stream the persisted journal to the console

// Human-readable esp_reset_reason_t name ("power-on", "task-watchdog", ...).
const char* resetReasonName(int reason);

#endif // EVENTS_H
