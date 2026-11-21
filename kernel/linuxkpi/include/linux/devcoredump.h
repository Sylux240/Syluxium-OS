/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
#define dev_coredumpv(dev, data, len, gfp) vfree(data)
#define dev_coredumpm(dev, owner, data, len, gfp, read, free) do { } while (0)
