/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
struct pinctrl_map { const char *dev_name; };
#define PIN_MAP_MUX_GROUP(dev, state, pinctrl, grp, func) { .dev_name = dev }
#define pinctrl_register_mappings(m, n) 0
#define pinctrl_unregister_mappings(m) do { } while (0)
