/* LinuxKPI: 64-bit statistics - plain counters on 64-bit CPUs. */
#ifndef _LKPI_LINUX_U64_STATS_SYNC_H
#define _LKPI_LINUX_U64_STATS_SYNC_H

#include <linux/types.h>

struct u64_stats_sync { int unused; };
#define u64_stats_init(s) do { (void)(s); } while (0)
#define u64_stats_update_begin(s) do { (void)(s); } while (0)
#define u64_stats_update_end(s) do { (void)(s); } while (0)
#define u64_stats_update_begin_irqsave(s) ((void)(s), 0UL)
#define u64_stats_update_end_irqrestore(s, f) do { (void)(s); (void)(f); } while (0)
#define u64_stats_fetch_begin(s) ((void)(s), 0u)
#define u64_stats_fetch_retry(s, start) ((void)(s), (void)(start), 0)
typedef struct { u64 v; } u64_stats_t;
#define u64_stats_read(p) ((p)->v)
#define u64_stats_set(p, val) ((p)->v = (val))
#define u64_stats_add(p, val) ((p)->v += (val))
#define u64_stats_inc(p) ((p)->v++)
#define u64_stats_sub(p, val) ((p)->v -= (val))

#endif
