/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
struct mfd_cell { const char *name; int id; int num_resources; const struct resource *resources; void *platform_data; size_t pdata_size; };
#define mfd_add_hotplug_devices(parent, cells, n) (-ENODEV)
#define mfd_add_devices(parent, id, cells, n, mem, irq, domain) (-ENODEV)
#define mfd_remove_devices(parent) do { } while (0)
