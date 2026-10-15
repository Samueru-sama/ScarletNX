/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* time: clocks and the time zone */
#include "hle/services/services_int.h"
#include "runtime/clock.h"
#include "hle/services/time.swipc.inc"
/* ================================================================ time */
/* The steady clock is the guest clock (guest_ns, the time base of the ticks the game reads) in seconds, with
   g_clock_id as its source id. The system clocks are a SystemClockContext {i64 offset; SteadyClockTimePoint
   {i64 time; u8 source_id[16]}} on it: the time is offset + the steady time, and offset is set once at startup
   from the host's clock, so the IPC answers and the shared memory agree */
static struct {
    KShmem* shm;
    i64 tz_off;  /* local - UTC, seconds: the guest's wall clock is the host's local time */
    i64 sys_off; /* the system clocks' offset */
} timesvc;
static u8 g_clock_id[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static i64 steady_s(void) { return (i64)(guest_ns() / 1000000000ull); }
static void sysclock_ctx(nn_time_SystemClockContext* ctx) {
    ctx->offset = timesvc.sys_off;
    ctx->timestamp.value = steady_s();
    memcpy(ctx->timestamp.source_id.v, g_clock_id, 16);
}
/* the time a SystemClockContext gives now: 0 (and failure) when it is on another steady clock */
static int ctx_time(const nn_time_SystemClockContext* ctx, i64* t) {
    if (memcmp(ctx->timestamp.source_id.v, g_clock_id, 16)) {
        *t = 0;
        return 0;
    }
    *t = ctx->offset + steady_s();
    return 1;
}
static int nn_timesrv_detail_service_ISystemClock_GetCurrentTime(SvcObj* self, nn_time_PosixTime* time) {
    *time = timesvc.sys_off + steady_s();
    return 0;
}
static int nn_timesrv_detail_service_ISystemClock_GetSystemClockContext(SvcObj* self,
                                                                        nn_time_SystemClockContext* context) {
    sysclock_ctx(context);
    return 0;
}
static int nn_timesrv_detail_service_ISystemClock_GetOperationEventReadableHandle(SvcObj* self, u32* event) {
    return new_event(event, "clock op", 0);
}
static int nn_timesrv_detail_service_ISteadyClock_GetCurrentTimePoint(SvcObj* self,
                                                                      nn_time_SteadyClockTimePoint* time_point) {
    time_point->value = steady_s();
    memcpy(time_point->source_id.v, g_clock_id, 16);
    return 0;
}
static i64 days_from_civil(i64 y, unsigned m, unsigned d) {
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (i64)doe - 719468;
}
/* the CalendarTime and CalendarAdditionalInfo of a local time t */
static void to_calendar(i64 t, nn_time_CalendarTime* cal, nn_time_sf_CalendarAdditionalInfo* info) {
    i64 days = t / 86400, rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    int hh = (int)(rem / 3600), mm = (int)(rem % 3600 / 60), ss = (int)(rem % 60);
    /* days -> y/m/d */
    i64 z = days + 719468;
    i64 era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 y = (i64)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    memset(cal, 0, sizeof *cal);
    cal->year = (i16)y;
    cal->month = (i8)m;
    cal->day = (i8)d;
    cal->hour = (i8)hh;
    cal->minute = (i8)mm;
    cal->second = (i8)ss;
    memset(info, 0, sizeof *info);
    info->day_of_week = (i32)((days % 7 + 11) % 7);
    info->day_of_year = (i32)(days - days_from_civil(y, 1, 1));
    memcpy(info->time_zone_name, "UTC", 4);
    info->utc_offset = (i32)timesvc.tz_off;
}
static int nn_timesrv_detail_service_ITimeZoneService_GetDeviceLocationName(SvcObj* self, nn_time_LocationName* name) {
    memcpy(name->v, "UTC", 4);
    return 0;
}
static int nn_timesrv_detail_service_ITimeZoneService_GetTotalLocationNameCount(SvcObj* self, i32* count) {
    *count = 1;
    return 0;
}
/* the one location, "UTC" */
static int nn_timesrv_detail_service_ITimeZoneService_LoadLocationNameList(SvcObj* self, i32 index, IpcBuf names,
                                                                           i32* count) {
    *count = index == 0 && names.addr && names.size >= 0x24;
    if (*count) {
        memset(GP(names.addr), 0, 0x24);
        memcpy(GP(names.addr), "UTC", 4);
    }
    return 0;
}
static int nn_timesrv_detail_service_ITimeZoneService_LoadTimeZoneRule(SvcObj* self, const nn_time_LocationName* name,
                                                                       IpcBuf rule) {
    if (rule.addr) memset(GP(rule.addr), 0, rule.size);
    return 0;
}
static int nn_timesrv_detail_service_ITimeZoneService_GetTimeZoneRuleVersion(SvcObj* self,
                                                                             nn_time_TimeZoneRuleVersion* version) {
    memcpy(version->v, "2021a", 6);
    return 0;
}
/* guest wall clock = host local time, whatever rule is passed */
static int nn_timesrv_detail_service_ITimeZoneService_ToCalendarTime(SvcObj* self, nn_time_PosixTime time, IpcBuf rule,
                                                                     nn_time_CalendarTime* calendar,
                                                                     nn_time_sf_CalendarAdditionalInfo* info) {
    to_calendar(time + timesvc.tz_off, calendar, info);
    return 0;
}
static int nn_timesrv_detail_service_ITimeZoneService_ToCalendarTimeWithMyRule(
    SvcObj* self, nn_time_PosixTime time, nn_time_CalendarTime* calendar, nn_time_sf_CalendarAdditionalInfo* info) {
    to_calendar(time + timesvc.tz_off, calendar, info);
    return 0;
}
/* the rough inverse: one time */
static i32 to_posix(const nn_time_CalendarTime* cal, IpcBuf times) {
    i64 t = days_from_civil((u16)cal->year, (u8)cal->month, (u8)cal->day) * 86400 + (u8)cal->hour * 3600 +
            (u8)cal->minute * 60 + (u8)cal->second;
    t -= timesvc.tz_off;
    if (times.addr && times.size >= 8) gw64(times.addr, (u64)t);
    return 1;
}
static int nn_timesrv_detail_service_ITimeZoneService_ToPosixTime(SvcObj* self, const nn_time_CalendarTime* calendar,
                                                                  IpcBuf rule, IpcBuf times, i32* count) {
    *count = to_posix(calendar, times);
    return 0;
}
static int nn_timesrv_detail_service_ITimeZoneService_ToPosixTimeWithMyRule(SvcObj* self,
                                                                            const nn_time_CalendarTime* calendar,
                                                                            IpcBuf times, i32* count) {
    *count = to_posix(calendar, times);
    return 0;
}
static void time_shm_update(void) {
    u8* p = kshmem_ptr(timesvc.shm);
    /* steady clock {i64 base_time_ns; u8 id[16]} x2 @0x08/0x20; system clocks {i64 offset; i64 time_point; u8 id[16]} x2 */
    u32 cnt = 0;
    memcpy(p, &cnt, 4);
    for (int k = 0; k < 2; k++) {
        i64 base = 0;
        memcpy(p + 8 + k * 0x18, &base, 8);
        memcpy(p + 8 + k * 0x18 + 8, g_clock_id, 16);
    }
    for (int c2 = 0; c2 < 2; c2++) {
        u8* q = p + (c2 ? 0x80 : 0x38);
        memcpy(q, &cnt, 4);
        for (int k = 0; k < 2; k++) {
            u8* e = q + 8 + k * 0x20;
            i64 tp = 0;
            memcpy(e, &timesvc.sys_off, 8);
            memcpy(e + 8, &tp, 8);
            memcpy(e + 16, g_clock_id, 16);
        }
    }
    /* auto correction {u32 counter; bool[2]} @0xC8 */
    memcpy(p + 0xC8, &cnt, 4);
    p[0xCC] = 1;
    p[0xCD] = 1;
    /* ContinuousAdjustmentTimePoint (17.0+) {u32 counter; pad; 2 x {rtc_offset, diff_scale, shift+pad, lower{t,id}, upper{t,id}}} @0xD0 */
    memcpy(p + 0xD0, &cnt, 4);
    for (int k = 0; k < 2; k++) {
        u8* e = p + 0xD8 + k * 0x48;
        memset(e, 0, 0x48);
        memcpy(e + 0x18 + 8, g_clock_id, 16);
        memcpy(e + 0x30 + 8, g_clock_id, 16);
    }
}
/* a ClockSnapshot of two contexts. A user context on another steady clock fails with TimeMismatch, a network one
   gives time 0 */
static int clock_snapshot(IpcBuf snapshot, const nn_time_SystemClockContext* user,
                          const nn_time_SystemClockContext* net, u8 type) {
    if (!snapshot.addr || snapshot.size < 0xD0) return RC(116, 1);
    u8 b[0xD0];
    nn_time_CalendarTime cal;
    nn_time_sf_CalendarAdditionalInfo info;
    memset(b, 0, sizeof b);
    memcpy(b, user, 0x20);
    memcpy(b + 0x20, net, 0x20);
    i64 ut, nt;
    if (!ctx_time(user, &ut)) return RC(116, 102); /* TimeMismatch */
    ctx_time(net, &nt);
    memcpy(b + 0x40, &ut, 8);
    memcpy(b + 0x48, &nt, 8);
    to_calendar(ut + timesvc.tz_off, &cal, &info);
    memcpy(b + 0x50, &cal, 8);
    memcpy(b + 0x60, &info, 0x18);
    to_calendar(nt + timesvc.tz_off, &cal, &info);
    memcpy(b + 0x58, &cal, 8);
    memcpy(b + 0x78, &info, 0x18);
    i64 tp = steady_s();
    memcpy(b + 0x90, &tp, 8);
    memcpy(b + 0x98, g_clock_id, 16);
    memcpy(b + 0xA8, "UTC", 4);
    b[0xCC] = 1;
    b[0xCD] = type;
    memcpy(GP(snapshot.addr), b, sizeof b);
    return 0;
}
/* the two ClockSnapshots of CalculateStandardUserSystemClockDifferenceByUser and CalculateSpanBetween */
static int two_snapshots(IpcBuf a, IpcBuf b, u8 sa[0xD0], u8 sb[0xD0]) {
    if (!a.addr || !b.addr || a.size < 0xD0 || b.size < 0xD0) return 0;
    memcpy(sa, GP(a.addr), 0xD0);
    memcpy(sb, GP(b.addr), 0xD0);
    return 1;
}
static SvcObj* system_clock(void) { return nn_timesrv_detail_service_ISystemClock_new(0); }
static int nn_timesrv_detail_service_IStaticService_GetStandardUserSystemClock(SvcObj* self, SvcObj** clock) {
    *clock = system_clock();
    return 0;
}
static int nn_timesrv_detail_service_IStaticService_GetStandardNetworkSystemClock(SvcObj* self, SvcObj** clock) {
    *clock = system_clock();
    return 0;
}
static int nn_timesrv_detail_service_IStaticService_GetStandardLocalSystemClock(SvcObj* self, SvcObj** clock) {
    *clock = system_clock();
    return 0;
}
static int nn_timesrv_detail_service_IStaticService_GetEphemeralNetworkSystemClock(SvcObj* self, SvcObj** clock) {
    *clock = system_clock();
    return 0;
}
static int nn_timesrv_detail_service_IStaticService_GetStandardSteadyClock(SvcObj* self, SvcObj** clock) {
    *clock = nn_timesrv_detail_service_ISteadyClock_new(0);
    return 0;
}
static int nn_timesrv_detail_service_IStaticService_GetTimeZoneService(SvcObj* self, SvcObj** service) {
    *service = nn_timesrv_detail_service_ITimeZoneService_new(0);
    return 0;
}
static int nn_timesrv_detail_service_IStaticService_GetSharedMemoryNativeHandle(SvcObj* self, u32* shared_memory) {
    *shared_memory = h_new(&timesvc.shm->o);
    return 0;
}
static int nn_timesrv_detail_service_IStaticService_GetStandardNetworkClockOperationEventReadableHandle(SvcObj* self,
                                                                                                        u32* event) {
    return new_event(event, "clock op", 0);
}
static int nn_timesrv_detail_service_IStaticService_GetEphemeralNetworkClockOperationEventReadableHandle(SvcObj* self,
                                                                                                         u32* event) {
    return new_event(event, "clock op", 0);
}
static int nn_timesrv_detail_service_IStaticService_IsStandardUserSystemClockAutomaticCorrectionEnabled(SvcObj* self,
                                                                                                        u8* enabled) {
    *enabled = 1;
    return 0;
}
static int nn_timesrv_detail_service_IStaticService_GetStandardUserSystemClockInitialYear(SvcObj* self, i32* year) {
    *year = 2019;
    return 0;
}
static int nn_timesrv_detail_service_IStaticService_IsStandardNetworkSystemClockAccuracySufficient(SvcObj* self,
                                                                                                   u8* sufficient) {
    *sufficient = 1;
    return 0;
}
/* the context's offset plus the steady clock's time less the seconds since boot. This steady clock counts the
   seconds since boot, so that is the offset */
static int nn_timesrv_detail_service_IStaticService_CalculateMonotonicSystemClockBaseTimePoint(
    SvcObj* self, const nn_time_SystemClockContext* context, i64* time) {
    if (memcmp(context->timestamp.source_id.v, g_clock_id, 16)) return RC(116, 102); /* TimeMismatch */
    *time = context->offset;
    return 0;
}
static int nn_timesrv_detail_service_IStaticService_GetClockSnapshot(SvcObj* self, u8 type, IpcBuf snapshot) {
    nn_time_SystemClockContext user, net;
    sysclock_ctx(&user);
    sysclock_ctx(&net);
    return clock_snapshot(snapshot, &user, &net, type);
}
static int nn_timesrv_detail_service_IStaticService_GetClockSnapshotFromSystemClockContext(
    SvcObj* self, u8 type, const nn_time_SystemClockContext* user_context,
    const nn_time_SystemClockContext* network_context, IpcBuf snapshot) {
    return clock_snapshot(snapshot, user_context, network_context, type);
}
/* the TimeSpan (ns) between the user contexts */
static int nn_timesrv_detail_service_IStaticService_CalculateStandardUserSystemClockDifferenceByUser(
    SvcObj* self, IpcBuf a, IpcBuf b, nn_TimeSpanType* difference) {
    u8 sa[0xD0], sb[0xD0];
    if (!two_snapshots(a, b, sa, sb)) return RC(116, 1);
    i64 oa, ob;
    memcpy(&oa, sa, 8);
    memcpy(&ob, sb, 8);
    int same = !memcmp(sa + 16, sb + 16, 16), auto_corrected = sa[0xCC] && sb[0xCC];
    *difference = same && !auto_corrected ? (ob - oa) * 1000000000ll : 0;
    return 0;
}
/* the TimeSpan (ns) between the steady time points, or the network times */
static int nn_timesrv_detail_service_IStaticService_CalculateSpanBetween(SvcObj* self, IpcBuf a, IpcBuf b,
                                                                         nn_TimeSpanType* span) {
    u8 sa[0xD0], sb[0xD0];
    if (!two_snapshots(a, b, sa, sb)) return RC(116, 1);
    i64 ta, tb;
    if (!memcmp(sa + 0x98, sb + 0x98, 16)) {
        memcpy(&ta, sa + 0x90, 8);
        memcpy(&tb, sb + 0x90, 8);
    } else {
        memcpy(&ta, sa + 0x48, 8);
        memcpy(&tb, sb + 0x48, 8);
        if (!ta || !tb) return RC(116, 102); /* TimeMismatch */
    }
    *span = (tb - ta) * 1000000000ll;
    return 0;
}
SvcObj* new_time(void) { return nn_timesrv_detail_service_IStaticService_new(0); }
/* at startup, before any session: the clocks' offsets and the shared memory, which the service keeps for the whole
   run (its reference keeps it alive when the game closes its handle) */
void time_init(void) {
    timesvc.tz_off = plat_tz_offset();
    LOG("time: PC time zone UTC%+.1f\n", timesvc.tz_off / 3600.0);
    timesvc.sys_off = plat_unix_time() - steady_s();
    timesvc.shm = kshmem_new(0x1000, "time");
    kobj_ref(&timesvc.shm->o);
    time_shm_update();
}
