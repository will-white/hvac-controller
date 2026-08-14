#include "setpoints.h"
#include <Arduino.h>
#include <cmath>
#include "config.h"
#include "events.h"
#include "hvac_logic.h"
#include "learning.h"
#include "main.h"
#include "net_time.h"

// -- Temporary hold state (RAM only; deliberately not persisted) --
static bool holdOn = false;
static float holdHeatF = 0, holdCoolF = 0;
static unsigned long holdUntilMs = 0;

// -- Protection latches (hysteresis between trigger and target) --
static bool freezeProtectActive = false;
static bool heatProtectActive = false;

static ScheduleEntry scheduleBase(const TimeInfo& now, bool& carriedOver) {
  ScheduleEntry* daySchedule;
  int entryCount;
  if (now.dayOfWeek >= 1 && now.dayOfWeek <= 5) { daySchedule = programSchedule.weekday; entryCount = programSchedule.weekdayEntryCount; }
  else { daySchedule = programSchedule.weekend; entryCount = programSchedule.weekendEntryCount; }

  ScheduleEntry active;
  bool entryStarted = false;
  for (int i = 0; i < entryCount; i++) {
    if (now.hour > daySchedule[i].startHour || (now.hour == daySchedule[i].startHour && now.minute >= daySchedule[i].startMinute)) {
      active = daySchedule[i];
      entryStarted = true;
    }
  }

  carriedOver = !entryStarted;
  if (!entryStarted) {
    // Before today's first period, carry over the last period of the previous
    // day (5 AM Monday should still honor Sunday night's Sleep setback).
    bool yesterdayWasWeekday = (now.dayOfWeek >= 2 && now.dayOfWeek <= 6);
    if (yesterdayWasWeekday) active = programSchedule.weekday[programSchedule.weekdayEntryCount - 1];
    else                     active = programSchedule.weekend[programSchedule.weekendEntryCount - 1];
  }
  return active;
}

// Smart recovery: if the NEXT schedule entry steps the target up (heat) or
// down (cool), start early using the learned °F/hr rate for the current
// season/outdoor bin, so the house arrives AT the target when the entry
// starts. Falls back to a conservative default rate until data accumulates.
static void applySmartRecovery(ResolvedTargets& r, const TimeInfo& now, float indoorF, float outdoorF) {
  if (!ENABLE_SMART_RECOVERY || isnan(indoorF)) return;
  if (r.source != SRC_SCHEDULE && r.source != SRC_CARRYOVER) return;

  // Next entry: today's first not-yet-started, else tomorrow's first.
  bool todayWeekday = (now.dayOfWeek >= 1 && now.dayOfWeek <= 5);
  ScheduleEntry* day = todayWeekday ? programSchedule.weekday : programSchedule.weekend;
  int count = todayWeekday ? programSchedule.weekdayEntryCount : programSchedule.weekendEntryCount;
  int nowMins = now.hour * 60 + now.minute;
  ScheduleEntry next;
  int minutesUntil = -1;
  for (int i = 0; i < count; i++) {
    int startMins = day[i].startHour * 60 + day[i].startMinute;
    if (startMins > nowMins) { next = day[i]; minutesUntil = startMins - nowMins; break; }
  }
  if (minutesUntil < 0) {
    int tomorrowDow = (now.dayOfWeek + 1) % 7;
    bool tomorrowWeekday = (tomorrowDow >= 1 && tomorrowDow <= 5);
    ScheduleEntry* tday = tomorrowWeekday ? programSchedule.weekday : programSchedule.weekend;
    int tcount = tomorrowWeekday ? programSchedule.weekdayEntryCount : programSchedule.weekendEntryCount;
    if (tcount == 0) return;
    next = tday[0];
    minutesUntil = (24 * 60 - nowMins) + next.startHour * 60 + next.startMinute;
  }
  if (minutesUntil > MAX_RECOVERY_TIME_MINS) return;

  int season = getCurrentSeason();
  int bin = getPerformanceBin(isnan(outdoorF) ? indoorF : outdoorF);

  float heatRise = next.heatSetpointF - indoorF;
  if (next.heatSetpointF > r.heatSetpointF && heatRise > 0) {
    float rate = performance.heatRate[season][bin];
    if (performance.heatSamples[season][bin] < 1 || rate < 0.5) rate = DEFAULT_RECOVERY_RATE_F_PER_HR;
    int leadMins = (int)(heatRise / rate * 60.0f);
    if (leadMins > MAX_RECOVERY_TIME_MINS) leadMins = MAX_RECOVERY_TIME_MINS;
    if (minutesUntil <= leadMins) { r.heatSetpointF = next.heatSetpointF; r.source = SRC_RECOVERY; }
  }

  float coolDrop = indoorF - next.coolSetpointF;
  if (next.coolSetpointF < r.coolSetpointF && coolDrop > 0) {
    float rate = performance.coolRate[season][bin];
    if (performance.coolSamples[season][bin] < 1 || rate < 0.5) rate = DEFAULT_RECOVERY_RATE_F_PER_HR;
    int leadMins = (int)(coolDrop / rate * 60.0f);
    if (leadMins > MAX_RECOVERY_TIME_MINS) leadMins = MAX_RECOVERY_TIME_MINS;
    if (minutesUntil <= leadMins) { r.coolSetpointF = next.coolSetpointF; r.source = SRC_RECOVERY; }
  }
}

ResolvedTargets resolveTargets(float indoorF, float outdoorF) {
  ResolvedTargets r;
  r.effectiveMode = systemMode;
  r.fanMode = FAN_AUTO;

  // Layer 1: base target pair.
  if (vacationModeActive) {
    r.heatSetpointF = programSchedule.vacation.heatSetpointF;
    r.coolSetpointF = programSchedule.vacation.coolSetpointF;
    r.fanMode = programSchedule.vacation.fanMode;
    r.source = SRC_VACATION;
  } else if (holdOn && currentTime() < holdUntilMs) {
    r.heatSetpointF = holdHeatF;
    r.coolSetpointF = holdCoolF;
    r.source = SRC_HOLD;
  } else if (!isTimeValid()) {
    // Clock never synced: the schedule is meaningless, hold something safe.
    r.heatSetpointF = NO_TIME_FALLBACK_HEAT_F;
    r.coolSetpointF = NO_TIME_FALLBACK_COOL_F;
    r.source = SRC_NO_TIME_FALLBACK;
  } else {
    if (holdOn) { holdOn = false; logEvent("hold expired"); }
    bool carried = false;
    ScheduleEntry e = scheduleBase(getCurrentTime(), carried);
    r.heatSetpointF = e.heatSetpointF;
    r.coolSetpointF = e.coolSetpointF;
    r.fanMode = e.fanMode;
    r.source = carried ? SRC_CARRYOVER : SRC_SCHEDULE;
  }

  // Layer 2: smart recovery may pull the upcoming entry's target forward.
  if (isTimeValid() && !vacationModeActive) applySmartRecovery(r, getCurrentTime(), indoorF, outdoorF);

  // Layer 3: the pair must never overlap, or auto mode fights itself.
  if (r.coolSetpointF - r.heatSetpointF < MIN_SETPOINT_GAP_F) {
    r.coolSetpointF = r.heatSetpointF + MIN_SETPOINT_GAP_F;
  }

  // Layer 4: protection clamps outrank everything, in every mode (even OFF).
  if (!isnan(indoorF)) {
    if (indoorF < FREEZE_PROTECT_TRIGGER_F) freezeProtectActive = true;
    else if (indoorF >= FREEZE_PROTECT_TARGET_F) freezeProtectActive = false;

    if (indoorF > HEAT_PROTECT_TRIGGER_F) heatProtectActive = true;
    else if (indoorF <= HEAT_PROTECT_TARGET_F) heatProtectActive = false;

    if (freezeProtectActive) {
      r.heatSetpointF = FREEZE_PROTECT_TARGET_F;
      r.effectiveMode = SYS_HEAT;
      r.source = SRC_FREEZE_PROTECT;
    } else if (heatProtectActive) {
      r.coolSetpointF = HEAT_PROTECT_TARGET_F;
      r.effectiveMode = SYS_COOL;
      r.source = SRC_HEAT_PROTECT;
    }
  }

  return r;
}

void setHold(float heatF, float coolF, unsigned long hours) {
  if (isnan(heatF) || isnan(coolF) || heatF < 40 || coolF > 95 || heatF > coolF) {
    Serial.println("Hold rejected: need 40 <= heat <= cool <= 95.");
    return;
  }
  holdOn = true;
  holdHeatF = heatF;
  holdCoolF = coolF;
  holdUntilMs = currentTime() + hours * 3600UL * 1000UL;
  log_manual_adjustment(heatF); // feed the occupancy-learning log
  logEvent("hold set %.1f/%.1fF for %luh", heatF, coolF, hours);
}

void clearHold() {
  if (holdOn) logEvent("hold cleared");
  holdOn = false;
}

bool holdActive() { return holdOn && currentTime() < holdUntilMs; }

const char* targetSourceName(TargetSource s) {
  switch (s) {
    case SRC_SCHEDULE:         return "schedule";
    case SRC_CARRYOVER:        return "carryover(prev day)";
    case SRC_RECOVERY:         return "smart recovery";
    case SRC_VACATION:         return "vacation";
    case SRC_HOLD:             return "hold";
    case SRC_FREEZE_PROTECT:   return "FREEZE PROTECTION";
    case SRC_HEAT_PROTECT:     return "HEAT PROTECTION";
    case SRC_NO_TIME_FALLBACK: return "fallback(no clock)";
  }
  return "?";
}
