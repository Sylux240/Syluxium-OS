#ifndef _LKPI_LINUX_JIFFIES_H
#define _LKPI_LINUX_JIFFIES_H

#include <linux/kernel.h>

LKPI_EXTERN_C_BEGIN

/* HZ = 1000: one jiffy is one millisecond of Syluxium's monotonic clock. */
unsigned long lkpi_jiffies(void);
u64 lkpi_ktime_ns(void);
#define jiffies lkpi_jiffies()
#define jiffies_64 ((u64)lkpi_jiffies())
#define get_jiffies_64() ((u64)lkpi_jiffies())
#define MAX_SCHEDULE_TIMEOUT LONG_MAX
#define INITIAL_JIFFIES 0UL

#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_before(a, b) time_after(b, a)
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define time_before_eq(a, b) time_after_eq(b, a)
#define time_in_range(a, b, c) (time_after_eq(a, b) && time_before_eq(a, c))
#define time_is_before_jiffies(a) time_after(jiffies, a)
#define time_is_after_jiffies(a) time_before(jiffies, a)

static inline unsigned long msecs_to_jiffies(unsigned int m) { return m; }
static inline unsigned long usecs_to_jiffies(unsigned int u) { return (u + 999) / 1000; }
static inline unsigned int jiffies_to_msecs(unsigned long j) { return (unsigned int)j; }
static inline unsigned int jiffies_to_usecs(unsigned long j) { return (unsigned int)(j * 1000); }
static inline unsigned long round_jiffies(unsigned long j) { return j; }
static inline unsigned long round_jiffies_relative(unsigned long j) { return j; }
#define round_jiffies_up(j) (j)

/* ktime */
typedef s64 ktime_t;
static inline ktime_t ktime_get(void) { return (ktime_t)lkpi_ktime_ns(); }
#define ktime_get_boottime() ktime_get()
#define ktime_get_raw() ktime_get()
static inline s64 ktime_to_ns(ktime_t t) { return t; }
static inline s64 ktime_to_us(ktime_t t) { return t / 1000; }
static inline s64 ktime_to_ms(ktime_t t) { return t / 1000000; }
static inline ktime_t ns_to_ktime(u64 ns) { return (ktime_t)ns; }
static inline ktime_t ms_to_ktime(u64 ms) { return (ktime_t)(ms * 1000000); }
#define ktime_add(a, b) ((a) + (b))
#define ktime_sub(a, b) ((a) - (b))
#define ktime_add_ns(t, n) ((t) + (s64)(n))
#define ktime_add_us(t, u) ((t) + (s64)(u) * 1000)
#define ktime_add_ms(t, m) ((t) + (s64)(m) * 1000000)
#define ktime_us_delta(a, b) (((a) - (b)) / 1000)
#define ktime_ms_delta(a, b) (((a) - (b)) / 1000000)
#define ktime_compare(a, b) ((a) < (b) ? -1 : (a) > (b) ? 1 : 0)
#define ktime_after(a, b) ((a) > (b))
#define ktime_before(a, b) ((a) < (b))
#define ktime_get_ns() ((u64)lkpi_ktime_ns())
#define ktime_get_boottime_ns() ((u64)lkpi_ktime_ns())
#define ktime_get_real_ns() ((u64)lkpi_ktime_ns())
#define ktime_get_seconds() ((s64)(lkpi_ktime_ns() / 1000000000ULL))
#define NSEC_PER_SEC 1000000000L
#define NSEC_PER_MSEC 1000000L
#define NSEC_PER_USEC 1000L
#define USEC_PER_SEC 1000000L
#define USEC_PER_MSEC 1000L
#define MSEC_PER_SEC 1000L

LKPI_EXTERN_C_END

#endif
