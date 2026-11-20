/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: GPIOs - none (VLV/CHV DSI panels only). */
struct gpio_desc;
enum gpiod_flags { GPIOD_ASIS = 0, GPIOD_IN = 1, GPIOD_OUT_LOW = 3, GPIOD_OUT_HIGH = 7, GPIOD_OUT_LOW_OPEN_DRAIN = 11, GPIOD_OUT_HIGH_OPEN_DRAIN = 15 };
#define gpiod_get(dev, con, flags) ((struct gpio_desc *)ERR_PTR(-ENOENT))
#define gpiod_get_index(dev, con, idx, flags) ((struct gpio_desc *)ERR_PTR(-ENOENT))
#define devm_gpiod_get_optional(dev, con, flags) ((struct gpio_desc *)NULL)
#define gpiod_put(d) do { } while (0)
#define gpiod_set_value_cansleep(d, v) do { } while (0)
#define gpiod_set_value(d, v) do { } while (0)
#define gpiod_get_value(d) 0
#define gpiod_direction_output(d, v) 0
