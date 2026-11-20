/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
#define prefetch(x) __builtin_prefetch(x)
#define prefetchw(x) __builtin_prefetch(x, 1)
#define prefetch_range(addr, len) do { } while (0)
