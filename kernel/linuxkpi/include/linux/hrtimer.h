/* LinuxKPI: high-resolution timers on top of the timer thread (1 ms
 * resolution; expiry callbacks run on the LinuxKPI timer thread). */
#ifndef _LKPI_LINUX_HRTIMER_H
#define _LKPI_LINUX_HRTIMER_H
#include <linux/kernel.h>
#include <linux/timer.h>
LKPI_EXTERN_C_BEGIN
enum hrtimer_restart { HRTIMER_NORESTART, HRTIMER_RESTART };
enum hrtimer_mode {
    HRTIMER_MODE_ABS = 0x00, HRTIMER_MODE_REL = 0x01, HRTIMER_MODE_PINNED = 0x02, HRTIMER_MODE_SOFT = 0x04,
    HRTIMER_MODE_HARD = 0x08, HRTIMER_MODE_ABS_SOFT = 0x04, HRTIMER_MODE_REL_SOFT = 0x05,
    HRTIMER_MODE_ABS_PINNED = 0x02, HRTIMER_MODE_REL_PINNED = 0x03, HRTIMER_MODE_ABS_HARD = 0x08,
    HRTIMER_MODE_REL_HARD = 0x09, HRTIMER_MODE_REL_PINNED_SOFT = 0x07, HRTIMER_MODE_ABS_PINNED_SOFT = 0x06,
};
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_BOOTTIME 7
struct hrtimer {
    enum hrtimer_restart (*function)(struct hrtimer*);
    ktime_t expires;
    struct timer_list lkpi_timer;
    volatile int lkpi_active;
};
void hrtimer_init(struct hrtimer* timer, int clock_id, enum hrtimer_mode mode);
void hrtimer_setup(struct hrtimer* timer, enum hrtimer_restart (*function)(struct hrtimer*), int clock_id, enum hrtimer_mode mode);
void hrtimer_start(struct hrtimer* timer, ktime_t tim, const enum hrtimer_mode mode);
static inline void hrtimer_start_range_ns(struct hrtimer* timer, ktime_t tim, u64 range_ns, const enum hrtimer_mode mode) { hrtimer_start(timer, tim, mode); }
int hrtimer_cancel(struct hrtimer* timer);
int hrtimer_try_to_cancel(struct hrtimer* timer);
static inline bool hrtimer_is_queued(struct hrtimer* timer) { return timer->lkpi_active; }
static inline bool hrtimer_active(const struct hrtimer* timer) { return timer->lkpi_active; }
u64 hrtimer_forward(struct hrtimer* timer, ktime_t now, ktime_t interval);
static inline u64 hrtimer_forward_now(struct hrtimer* timer, ktime_t interval) { return hrtimer_forward(timer, ktime_get(), interval); }
static inline ktime_t hrtimer_get_expires(const struct hrtimer* timer) { return timer->expires; }
static inline void hrtimer_set_expires(struct hrtimer* timer, ktime_t time) { timer->expires = time; }
static inline ktime_t hrtimer_cb_get_time(struct hrtimer* timer) { return ktime_get(); }
static inline ktime_t hrtimer_get_remaining(const struct hrtimer* timer) { return timer->expires - ktime_get(); }
LKPI_EXTERN_C_END
#endif
