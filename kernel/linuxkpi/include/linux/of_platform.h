/* LinuxKPI: platform devices from a device tree (none on x86 PCs). */
#pragma once
#include <linux/lkpi_all.h>
struct device_node;
struct of_device_id;
static inline int of_platform_default_populate(struct device_node *root, const void *lookup, struct device *parent)
{
    return -ENODEV;
}
static inline int of_platform_populate(struct device_node *root, const struct of_device_id *matches,
                                       const void *lookup, struct device *parent)
{
    return -ENODEV;
}
