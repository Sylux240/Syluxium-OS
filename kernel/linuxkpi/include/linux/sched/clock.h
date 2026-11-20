/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
u64 lkpi_local_clock(void);
#define local_clock() lkpi_local_clock()
#define sched_clock() lkpi_local_clock()
#define running_clock() lkpi_local_clock()
#define cpu_clock(cpu) lkpi_local_clock()
