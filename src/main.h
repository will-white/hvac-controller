#ifndef MAIN_H
#define MAIN_H

#include "config.h"

// Global Variables needed by other files
extern Schedule programSchedule;
extern HvacPerformance performance;
extern bool systemLockedOut;
extern bool systemInFaultState;
extern bool vacationModeActive;
extern TempUnit tempUnit;
extern SystemMode systemMode;
// Last control-tick snapshot, read by the console `status` command.
extern ResolvedTargets lastResolved;
extern float lastIndoorF;

// Set when learning data changes; flushed to NVS on a timer (flash wear).
extern bool performanceDirty;

// Function Declarations needed by other files
TimeInfo getCurrentTime();
unsigned long currentTime();
void safeDelay(unsigned long ms);
void saveSettings();       // user settings + schedule (on change)
void savePerformance();    // learned data blob only (batched)
int getCurrentSeason();

#endif // MAIN_H
