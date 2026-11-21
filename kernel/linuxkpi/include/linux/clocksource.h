/* LinuxKPI: clock sources and cross time stamps (Linux clocksource.h /
 * timekeeping.h): what network drivers with PTP clocks refer to. No PTP
 * clocks are registered here, so the cross time stamp paths aren't used. */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/timecounter.h>

typedef int clockid_t;

static inline u64 mul_u64_u64_div_u64(u64 a, u64 b, u64 c)
{
    return (u64)(((unsigned __int128)a * b) / c);
}

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW 4
#endif
#ifndef ktime_get_raw_ts64
#define ktime_get_raw_ts64(ts) ktime_get_ts64(ts)
#endif

enum clocksource_ids { CSID_GENERIC = 0, CSID_ARM_ARCH_COUNTER, CSID_X86_TSC_EARLY, CSID_X86_TSC, CSID_X86_KVM_CLK,
                       CSID_X86_ART, CSID_MAX };

struct system_counterval_t {
    u64 cycles;
    enum clocksource_ids cs_id;
    bool use_nsecs;
};

struct system_time_snapshot {
    u64 cycles;
    ktime_t real;
    ktime_t raw;
    enum clocksource_ids cs_id;
    unsigned int clock_was_set_seq;
    u8 cs_was_changed_seq;
};

#ifndef _LKPI_SYSTEM_DEVICE_CROSSTSTAMP
#define _LKPI_SYSTEM_DEVICE_CROSSTSTAMP
struct system_device_crosststamp {
    ktime_t device;
    ktime_t sys_realtime;
    ktime_t sys_monoraw;
};
#endif

static inline void ktime_get_snapshot(struct system_time_snapshot *s) { memset(s, 0, sizeof(*s)); }
static inline int get_device_system_crosststamp(int (*get_time_fn)(ktime_t *device_time,
                                                                   struct system_counterval_t *system_counterval,
                                                                   void *ctx),
                                                void *ctx, struct system_time_snapshot *history,
                                                struct system_device_crosststamp *xtstamp)
{
    return -EOPNOTSUPP;
}
#ifndef ktime_get_clocktai
#define ktime_get_clocktai() ktime_get_real()
#endif
