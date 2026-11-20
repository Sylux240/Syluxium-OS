/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: stop_machine - run fn with the other CPUs out of the way.
 * Callers (i915: GGTT updates on old chipsets) only need it to be atomic
 * against concurrent access, which their own locking already gives here. */
typedef int (*cpu_stop_fn_t)(void *arg);
static inline int stop_machine(cpu_stop_fn_t fn, void *data, const void *cpus) { return fn(data); }
