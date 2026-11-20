/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
#define kernel_fpu_begin() do { } while (0)
#define kernel_fpu_end() do { } while (0)
#define irq_fpu_usable() false
