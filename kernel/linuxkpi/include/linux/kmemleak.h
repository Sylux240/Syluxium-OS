/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
#define kmemleak_update_trace(p) do { } while (0)
#define kmemleak_alloc(p, s, c, g) do { } while (0)
#define kmemleak_free(p) do { } while (0)
#define kmemleak_not_leak(p) do { } while (0)
#define kmemleak_ignore(p) do { } while (0)
