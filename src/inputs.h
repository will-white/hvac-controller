#ifndef INPUTS_H
#define INPUTS_H

#include <stdint.h>
#include "config.h"

// EVB opto-isolated inputs (OPT1-4): 24VAC equipment feedback (prove that
// heat/cool/fan calls are actually answered) and dry-contact sense (window).
// This is the input HAL — nothing else reads these GPIOs directly.
void inputs_begin();
void inputs_poll();                 // every loop(): sample + AC half-wave hold

bool inputs_isActive(int idx);      // debounced signal presence (real millis)
bool inputs_rawLevel(int idx);      // instantaneous GPIO level (diagnostics)
bool inputs_isCommissioned(int idx);// has EVER seen a signal (rules armed)
int  inputs_forRole(OptoRole role); // first input with this role, or -1

// Commissioned mask persists with the health counters so proving stays armed
// across reboots.
uint8_t inputs_commissionedMask();
void    inputs_setCommissioned(uint8_t mask);

const char* optoRoleName(OptoRole r);

#endif // INPUTS_H
