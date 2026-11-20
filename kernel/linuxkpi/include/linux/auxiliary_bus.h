/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
struct auxiliary_device { struct device dev; const char *name; u32 id; };
struct auxiliary_device_id { char name[40]; unsigned long driver_data; };
#define auxiliary_device_init(a) (-ENODEV)
#define __auxiliary_device_add(a, n) (-ENODEV)
#define auxiliary_device_add(a) (-ENODEV)
#define auxiliary_device_uninit(a) do { } while (0)
#define auxiliary_device_delete(a) do { } while (0)
