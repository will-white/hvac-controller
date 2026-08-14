#ifndef EVENTS_H
#define EVENTS_H

// In-RAM event ring buffer: relay transitions, faults, mode changes, service
// actions. Dumped by the console `log` command. (Flash-persisted journal is a
// Phase 5 roadmap item.)
void logEvent(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void dumpEvents();

#endif // EVENTS_H
