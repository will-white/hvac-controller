#ifndef HEALTH_H
#define HEALTH_H

// S.M.A.R.T.-style equipment health monitoring: attributes with thresholds,
// evaluated continuously, reported via the console `health` command. Rules are
// ADVISORY — they log and count, they never interrupt heating/cooling (a false
// positive killing heat in winter would be worse than any alert).
void health_begin();                 // load persisted counters; classify reset reason
void health_poll();                  // per control tick: no-response, filter hours, sensor dropouts
void health_reportCycleStart(bool isHeating);
void health_reportCycleRate(bool isHeating, float rateFPerHr, float baselineAvg, int baselineSamples);
void health_reportMaxRun(bool isHeating);
void health_flushIfDirty();          // piggybacks on the batched NVS flush policy
void health_resetFilter();           // console: after a filter change
void health_resetCounters();         // console: zero ALL counters (after servicing)
void printHealthReport();            // console `health`
int  health_warningCount();

// Pure rule evaluators, exposed for the boot self-tests.
bool health_evalNoResponse(bool isHeating, unsigned long elapsedMs, float startTempF, float nowTempF);
bool health_evalCapacity(float rateFPerHr, float baselineAvg, int baselineSamples);

#endif // HEALTH_H
