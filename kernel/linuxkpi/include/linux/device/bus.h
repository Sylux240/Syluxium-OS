/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
struct bus_type { const char *name; };
#define bus_register(b) 0
#define bus_unregister(b) do { } while (0)
#define bus_register_notifier(b, nb) 0
#define bus_unregister_notifier(b, nb) 0
#define BUS_NOTIFY_ADD_DEVICE 0x00000001
#define BUS_NOTIFY_DEL_DEVICE 0x00000002
#define BUS_NOTIFY_BIND_DRIVER 0x00000004
#define BUS_NOTIFY_BOUND_DRIVER 0x00000005
#define BUS_NOTIFY_UNBIND_DRIVER 0x00000006
#define BUS_NOTIFY_UNBOUND_DRIVER 0x00000007
#define BUS_NOTIFY_DRIVER_NOT_BOUND 0x00000008
