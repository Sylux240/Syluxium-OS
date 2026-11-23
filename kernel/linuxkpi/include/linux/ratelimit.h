/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/types.h>
#include <linux/spinlock.h>
struct ratelimit_state { int interval, burst, printed, missed; unsigned long begin, flags; spinlock_t lock; };
#define RATELIMIT_MSG_ON_RELEASE 1
#define DEFAULT_RATELIMIT_INTERVAL (5 * HZ)
#define DEFAULT_RATELIMIT_BURST 10
#define RATELIMIT_STATE_INIT(name, i, b) { .interval = (i), .burst = (b) }
#define DEFINE_RATELIMIT_STATE(name, i, b) struct ratelimit_state name = RATELIMIT_STATE_INIT(name, i, b)
#define ratelimit_state_init(rs, i, b) do { (rs)->interval = (i); (rs)->burst = (b); } while (0)
#define ratelimit_set_flags(rs, f) ((rs)->flags = (f))
#define ratelimit_state_exit(rs) do { } while (0)
/* glue/lib.c, with the state's own interval and burst (interval 0: no limit) */
int lkpi_ratelimit_state(struct ratelimit_state *rs);
#define __ratelimit(rs) lkpi_ratelimit_state(rs)
#define ___ratelimit(rs, func) lkpi_ratelimit_state(rs)
