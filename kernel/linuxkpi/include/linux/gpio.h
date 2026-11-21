/* LinuxKPI */
#pragma once
#include <linux/gpio/consumer.h>

/* The legacy GPIO number API: no GPIO lines are driven here (no GPIO
 * controller drivers), so requests fail and reads give 0. */
#define GPIOF_IN 0x1
#define GPIOF_OUT_INIT_LOW 0x0
#define GPIOF_OUT_INIT_HIGH 0x2
static inline int gpio_request_one(unsigned gpio, unsigned long flags, const char *label) { return -ENODEV; }
static inline int devm_gpio_request_one(struct device *dev, unsigned gpio, unsigned long flags, const char *label) { return -ENODEV; }
static inline void gpio_free(unsigned gpio) { }
static inline int gpio_get_value(unsigned gpio) { return 0; }
static inline void gpio_set_value(unsigned gpio, int value) { }
static inline bool gpio_is_valid(int gpio) { return false; }
