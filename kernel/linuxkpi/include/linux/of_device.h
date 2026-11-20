/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/of.h>
#define of_device_uevent_modalias(dev, env) (-ENODEV)
#define of_device_modalias(dev, buf, len) (-ENODEV)
#define of_driver_match_device(dev, drv) 0
#define of_dma_configure(dev, np, force) 0
