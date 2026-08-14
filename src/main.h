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

// Why the persisted fault/lockout latched ("" when neither is active).
extern char g_safetyReason[64];

// Function Declarations needed by other files
TimeInfo getCurrentTime();
unsigned long currentTime();
void safeDelay(unsigned long ms);
void saveSettings();       // user settings + schedule (on change)
void savePerformance();    // learned data blob only (batched)
// Persist fault/lockout (+ reason) to NVS IMMEDIATELY so safety latches survive
// reboots until explicitly acknowledged. Rare events — wear is negligible.
// Call with the trip reason when latching, nullptr when acknowledging.
void saveSafetyState(const char* reason);
int getCurrentSeason();

#endif // MAIN_H
