/* LinuxKPI: LED class. Syluxium has no LED subsystem yet: drivers can
 * register their LEDs (e.g. a Wi-Fi stick's activity light), which then
 * simply stay in the state the driver sets at probe time. */
#pragma once
#include <linux/lkpi_all.h>

enum led_brightness { LED_OFF = 0, LED_ON = 1, LED_HALF = 127, LED_FULL = 255 };

struct led_classdev {
	const char *name;
	unsigned int brightness;
	unsigned int max_brightness;
	int flags;
	void (*brightness_set)(struct led_classdev *led_cdev, enum led_brightness brightness);
	int (*brightness_set_blocking)(struct led_classdev *led_cdev, enum led_brightness brightness);
	enum led_brightness (*brightness_get)(struct led_classdev *led_cdev);
	const char *default_trigger;
	struct device *dev;
};

static inline int led_classdev_register(struct device *parent, struct led_classdev *led)
{
	(void)parent;
	(void)led;
	return 0;
}
static inline void led_classdev_unregister(struct led_classdev *led) { (void)led; }
static inline int devm_led_classdev_register(struct device *parent, struct led_classdev *led)
{
	return led_classdev_register(parent, led);
}
