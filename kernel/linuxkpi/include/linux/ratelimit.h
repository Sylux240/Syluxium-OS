/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/types.h>
struct ratelimit_state { int interval, burst, printed, missed; unsigned long begin, flags; };
#define RATELIMIT_MSG_ON_RELEASE 1
#define DEFAULT_RATELIMIT_INTERVAL (5 * HZ)
#define DEFAULT_RATELIMIT_BURST 10
#define RATELIMIT_STATE_INIT(name, i, b) { .interval = (i), .burst = (b) }
#define DEFINE_RATELIMIT_STATE(name, i, b) struct ratelimit_state name = RATELIMIT_STATE_INIT(name, i, b)
#define ratelimit_state_init(rs, i, b) do { (rs)->interval = (i); (rs)->burst = (b); } while (0)
#define ratelimit_set_flags(rs, f) ((rs)->flags = (f))
#define ratelimit_state_exit(rs) do { } while (0)
#define __ratelimit(rs) 1
#define ___ratelimit(rs, func) 1
