/* LinuxKPI: CPU hotplug - CPUs never go away here. */
#pragma once
#include <linux/lkpi_all.h>
static inline void cpus_read_lock(void) { }
static inline void cpus_read_unlock(void) { }
