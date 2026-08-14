#ifndef SETPOINTS_H
#define SETPOINTS_H

#include "config.h"

// Layered target resolution: vacation → hold → schedule/carryover (or the
// no-time fallback) → smart recovery → gap enforcement → freeze/heat
// protection clamps. The returned provenance answers "why is the target what
// it is?". outdoorF selects the learned-rate bin for recovery (NAN is fine).
ResolvedTargets resolveTargets(float indoorF, float outdoorF);

void setHold(float heatF, float coolF, unsigned long hours);
void clearHold();
bool holdActive();
const char* targetSourceName(TargetSource s);

#endif // SETPOINTS_H
