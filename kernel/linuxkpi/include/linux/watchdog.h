/* LinuxKPI: watchdog devices (no watchdog subsystem; Broadcom SoC headers
 * embed the type). */
#pragma once
#include <linux/lkpi_all.h>
struct watchdog_ops;
struct watchdog_info;
struct watchdog_device {
    int id;
    struct device *parent;
    const struct watchdog_info *info;
    const struct watchdog_ops *ops;
    unsigned int timeout, min_timeout, max_timeout;
    void *driver_data;
    unsigned long status;
};
