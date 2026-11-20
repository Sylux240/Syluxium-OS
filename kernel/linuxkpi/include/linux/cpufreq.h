/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: CPU frequency policies - none exposed. */
struct cpufreq_policy { unsigned int min, max, cur; struct { unsigned int max_freq, min_freq; } cpuinfo; };
#define cpufreq_cpu_get(cpu) ((struct cpufreq_policy *)NULL)
#define cpufreq_cpu_put(p) do { } while (0)
#define cpufreq_quick_get_max(cpu) 0U
#define cpufreq_quick_get(cpu) 0U
