/* LinuxKPI: kdump (never: Syluxium doesn't boot crash-dump kernels). */
#pragma once
#include <linux/types.h>
static inline bool is_kdump_kernel(void) { return false; }
