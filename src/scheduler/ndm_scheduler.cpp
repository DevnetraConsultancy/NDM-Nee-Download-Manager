#include "ndm_scheduler.h"

namespace {

const UINT64 kMinute = 60ULL * 10000000ULL;
const UINT64 kDay    = 1440ULL * kMinute;
const UINT64 kMaxSpan = 400ULL * kDay;

// 1601-01-01 was a Monday: day 0 -> weekday 1 (0 = Sunday).
int WeekdayOf(UINT64 dayIndex) { return (int)((dayIndex + 1) % 7); }

int DateOf(UINT64 dayIndex)
{
    ULARGE_INTEGER u;
    u.QuadPart = dayIndex * kDay;
    FILETIME ft = { u.LowPart, u.HighPart };
    SYSTEMTIME st = {};
    if (!FileTimeToSystemTime(&ft, &st)) return 0;
    return st.wYear * 10000 + st.wMonth * 100 + st.wDay;
}

bool StartDay(const NdmSchedule& s, UINT64 dayIndex)
{
    if (s.once) return s.onceDate > 0 && DateOf(dayIndex) == s.onceDate;
    return (s.dayMask & (1 << WeekdayOf(dayIndex))) != 0;
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The events that belong to day d (its start and its stop).
void EventsOfDay(const NdmSchedule& s, UINT64 d, UINT64 ev[2], int type[2], int& n)
{
    n = 0;
    if (!StartDay(s, d)) return;
    int startM = Clamp(s.startMinute, 0, 1439), stopM = Clamp(s.stopMinute, 0, 1439);
    if (s.startEnabled) { ev[n] = d * kDay + (UINT64)startM * kMinute; type[n++] = NDM_SCHED_START; }
    if (s.stopEnabled)
    {
        UINT64 stopDay = d + ((s.startEnabled && stopM <= startM) ? 1 : 0);
        ev[n] = stopDay * kDay + (UINT64)stopM * kMinute;
        type[n++] = NDM_SCHED_STOP;
    }
}

} // namespace

int NdmScheduleEventBetween(const NdmSchedule& s, UINT64 from, UINT64 to)
{
    if (to <= from || (!s.startEnabled && !s.stopEnabled)) return NDM_SCHED_NONE;
    if (to - from > kMaxSpan) from = to - kMaxSpan;
    UINT64 best = 0;
    int bestType = NDM_SCHED_NONE;
    // a stop on day d can belong to the start of day d-1
    UINT64 d0 = from / kDay;
    d0 = d0 ? d0 - 1 : 0;
    for (UINT64 d = d0; d <= to / kDay; ++d)
    {
        UINT64 ev[2]; int type[2]; int n = 0;
        EventsOfDay(s, d, ev, type, n);
        for (int i = 0; i < n; ++i)
            if (ev[i] > from && ev[i] <= to && ev[i] >= best)
            {
                // same instant: the stop wins (never leave things running)
                if (ev[i] == best && type[i] != NDM_SCHED_STOP) continue;
                best = ev[i];
                bestType = type[i];
            }
    }
    return bestType;
}

UINT64 NdmScheduleNextEvent(const NdmSchedule& s, UINT64 from)
{
    if (!s.startEnabled && !s.stopEnabled) return 0;
    UINT64 best = 0;
    UINT64 d0 = from / kDay;
    d0 = d0 ? d0 - 1 : 0;
    for (UINT64 d = d0; d <= (from + kMaxSpan) / kDay; ++d)
    {
        if (best && d * kDay > best) break;          // later days only have later events
        UINT64 ev[2]; int type[2]; int n = 0;
        EventsOfDay(s, d, ev, type, n);
        for (int i = 0; i < n; ++i)
            if (ev[i] > from && (!best || ev[i] < best)) best = ev[i];
    }
    return best;
}

UINT64 NdmLocalNowFt()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    FILETIME ft = {};
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

UINT64 NdmLocalFt(int year, int month, int day, int minuteOfDay)
{
    SYSTEMTIME st = {};
    st.wYear = (WORD)year; st.wMonth = (WORD)month; st.wDay = (WORD)day;
    FILETIME ft = {};
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart + (UINT64)Clamp(minuteOfDay, 0, 1439) * kMinute;
}
