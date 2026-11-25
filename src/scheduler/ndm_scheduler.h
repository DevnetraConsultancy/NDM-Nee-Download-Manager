#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../common/ndm_common.h"

// ---------------------------------------------------------------------------
// Scheduler math (no threads, no state). NdmQueue's core timer thread asks
// these functions which start / stop events of the NdmSchedule fell into the
// time that passed since its last check, and when the next one is due.
//
// Times are LOCAL wall-clock instants written as FILETIME-style UINT64
// (100 ns units since 1601-01-01 of the local calendar; NdmLocalNowFt()).
//
// Semantics (NdmSchedule in ndm_common.h):
//  * Daily mode: a start event at startMinute on every day whose bit is set
//    in dayMask (bit 0 = Sunday). Once mode: only on onceDate (yyyymmdd).
//  * A stop event belongs to the start day: at stopMinute of the same day,
//    or of the next day when stopMinute <= startMinute (overnight window,
//    e.g. 23:00 -> 07:00). With the start disabled, the stop is on the
//    selected days themselves.
//  * A computer that slept / was off through an event catches up when the
//    timer runs again; if a start and its stop both passed meanwhile, only
//    the later one (the stop) counts.
// ---------------------------------------------------------------------------

#define NDM_SCHED_NONE   0
#define NDM_SCHED_START  1
#define NDM_SCHED_STOP   2

// The latest event in (fromLocal, toLocal], or NDM_SCHED_NONE.
int    NdmScheduleEventBetween(const NdmSchedule& s, UINT64 fromLocal, UINT64 toLocal);

// Local time of the first event after fromLocal; 0 = none in the next 400 days.
UINT64 NdmScheduleNextEvent(const NdmSchedule& s, UINT64 fromLocal);

// Current local time as such a value.
UINT64 NdmLocalNowFt();

// The value for a local date + minute of day (y, m 1-12, d 1-31); 0 if invalid.
UINT64 NdmLocalFt(int year, int month, int day, int minuteOfDay);
