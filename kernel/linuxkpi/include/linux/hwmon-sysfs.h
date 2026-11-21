/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/hwmon.h>
struct sensor_device_attribute { struct device_attribute dev_attr; int index; };
#define to_sensor_dev_attr(a) container_of(a, struct sensor_device_attribute, dev_attr)
#define SENSOR_DEVICE_ATTR(_name, _mode, _show, _store, _index) struct sensor_device_attribute sensor_dev_attr_##_name = { .dev_attr = __ATTR(_name, _mode, _show, _store), .index = _index }
#define SENSOR_DEVICE_ATTR_RO(_name, _func, _index) SENSOR_DEVICE_ATTR(_name, 0444, _func##_show, NULL, _index)
#define SENSOR_DEVICE_ATTR_RW(_name, _func, _index) SENSOR_DEVICE_ATTR(_name, 0644, _func##_show, _func##_store, _index)
#define SENSOR_DEVICE_ATTR_WO(_name, _func, _index) SENSOR_DEVICE_ATTR(_name, 0200, NULL, _func##_store, _index)
