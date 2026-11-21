/* LinuxKPI: PTP hardware clocks (Linux's ptp_clock_kernel.h, PTP support
 * not configured): registration reports "not available", as Linux does
 * without CONFIG_PTP_1588_CLOCK. */
#pragma once

#include <linux/lkpi_all.h>

struct ptp_clock;
struct ptp_system_timestamp;
struct ptp_clock_request;

struct system_device_crosststamp {
    ktime_t device;
    ktime_t sys_realtime;
    ktime_t sys_monoraw;
};

struct ptp_clock_info {
    struct module *owner;
    char name[32];
    s32 max_adj;
    int n_alarm, n_ext_ts, n_per_out, n_pins, pps;
    int (*adjfine)(struct ptp_clock_info *ptp, long scaled_ppm);
    int (*adjphase)(struct ptp_clock_info *ptp, s32 phase);
    int (*adjtime)(struct ptp_clock_info *ptp, s64 delta);
    int (*gettime64)(struct ptp_clock_info *ptp, struct timespec64 *ts);
    int (*gettimex64)(struct ptp_clock_info *ptp, struct timespec64 *ts, struct ptp_system_timestamp *sts);
    int (*getcrosststamp)(struct ptp_clock_info *ptp, struct system_device_crosststamp *cts);
    int (*settime64)(struct ptp_clock_info *p, const struct timespec64 *ts);
    int (*enable)(struct ptp_clock_info *ptp, struct ptp_clock_request *request, int on);
    int (*verify)(struct ptp_clock_info *ptp, unsigned int pin, int func, unsigned int chan);
    long (*do_aux_work)(struct ptp_clock_info *ptp);
};

static inline struct ptp_clock *ptp_clock_register(struct ptp_clock_info *info, struct device *parent) { return NULL; }
static inline int ptp_clock_unregister(struct ptp_clock *ptp) { return 0; }
static inline int ptp_clock_index(struct ptp_clock *ptp) { return -1; }

static inline long scaled_ppm_to_ppb(long ppm)
{
    s64 ppb = 1 + ppm;
    ppb *= 125;
    ppb >>= 13;
    return (long)ppb;
}

static inline u64 adjust_by_scaled_ppm(u64 base, long scaled_ppm)
{
    bool negative = false;
    u64 adj;
    if (scaled_ppm < 0) {
        negative = true;
        scaled_ppm = -scaled_ppm;
    }
    adj = (base * (u64)scaled_ppm) / (1000000ULL << 16);
    return negative ? base - adj : base + adj;
}
