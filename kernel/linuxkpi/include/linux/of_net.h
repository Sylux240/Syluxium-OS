/* LinuxKPI: MAC addresses from the device tree (there is none on PCs). */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/of.h>

static inline int of_get_mac_address(struct device_node *np, u8 *mac) { return -ENODEV; }
static inline int of_get_mac_address_nvmem(struct device_node *np, u8 *mac) { return -ENODEV; }
static inline int of_get_phy_mode(struct device_node *np, int *mode) { return -ENODEV; }
