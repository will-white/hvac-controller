#ifndef CONSOLE_H
#define CONSOLE_H

#include "config.h"

// Serial (USB-C CDC) debug & service console. Also owns:
//  - the virtual clock (sim speed multiplier) that currentTime() reads
//  - sensor/clock simulation overrides
//  - service (test) mode state and its safety timers
void console_begin();
void console_poll();

unsigned long console_virtualMillis();
bool console_testModeActive();       // true = normal control suspended

// Simulation overrides; each returns true and fills `out` when active.
bool console_simTemp(float& outF);
bool console_simOutdoor(float& outF);
bool console_simHumidity(float& out);
bool console_simTime(TimeInfo& out);

#endif // CONSOLE_H
