/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
#define tegra_sku_info (*(struct { int gpu_speedo_id, gpu_speedo_value, gpu_process_id; } *)0)
