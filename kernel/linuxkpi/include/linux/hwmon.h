/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: hardware monitoring - sensors are not exposed. */
struct hwmon_chip_info;
#define hwmon_device_register_with_groups(dev, name, drvdata, groups) ((struct device *)ERR_PTR(-ENODEV))
#define devm_hwmon_device_register_with_groups(dev, name, drvdata, groups) ((struct device *)ERR_PTR(-ENODEV))
#define hwmon_device_register_with_info(dev, name, drvdata, chip, groups) ((struct device *)ERR_PTR(-ENODEV))
#define hwmon_device_unregister(dev) do { } while (0)
