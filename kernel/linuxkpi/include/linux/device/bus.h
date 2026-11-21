/* LinuxKPI: buses with drivers that bind to their devices by match()
 * (phylib's MDIO bus). PCI and USB have their own binding (pci.cpp, usb.cpp);
 * these are for buses Linux code registers itself (glue/devmodel.c). */
#pragma once
#include <linux/lkpi_all.h>

struct kobj_uevent_env;
struct bus_type {
    const char *name;
    /* appended */
    const char *dev_name;
    const struct attribute_group **bus_groups;
    const struct attribute_group **dev_groups;
    const struct attribute_group **drv_groups;
    int (*match)(struct device *dev, const struct device_driver *drv);
    int (*uevent)(const struct device *dev, struct kobj_uevent_env *env);
    int (*probe)(struct device *dev);
    void (*sync_state)(struct device *dev);
    void (*remove)(struct device *dev);
    void (*shutdown)(struct device *dev);
    const struct dev_pm_ops *pm;
};

int bus_register(const struct bus_type *bus);
void bus_unregister(const struct bus_type *bus);
int bus_for_each_dev(const struct bus_type *bus, struct device *start, void *data, int (*fn)(struct device *, void *));
struct device *bus_find_device(const struct bus_type *bus, struct device *start, const void *data,
                               int (*match)(struct device *dev, const void *data));
struct device *bus_find_device_by_name(const struct bus_type *bus, struct device *start, const char *name);
#define bus_register_notifier(b, nb) 0
#define bus_unregister_notifier(b, nb) 0
#define BUS_NOTIFY_ADD_DEVICE 0x00000001
#define BUS_NOTIFY_DEL_DEVICE 0x00000002
#define BUS_NOTIFY_BIND_DRIVER 0x00000004
#define BUS_NOTIFY_BOUND_DRIVER 0x00000005
#define BUS_NOTIFY_UNBIND_DRIVER 0x00000006
#define BUS_NOTIFY_UNBOUND_DRIVER 0x00000007
#define BUS_NOTIFY_DRIVER_NOT_BOUND 0x00000008
