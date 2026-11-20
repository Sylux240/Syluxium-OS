/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: perf events - none (i915's PMU isn't built). */
struct perf_event;
struct pmu { const char *name; int type; };
struct perf_event_attr { u32 type; u64 config; };
#define perf_pmu_register(pmu, name, type) (-ENODEV)
#define perf_pmu_unregister(pmu) do { } while (0)
