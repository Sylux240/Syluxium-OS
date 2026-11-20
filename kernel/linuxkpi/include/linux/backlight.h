/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: backlight class devices. Drivers register their backlight
 * and set brightness through it; there's no sysfs to expose it yet. */
enum backlight_type { BACKLIGHT_RAW = 1, BACKLIGHT_PLATFORM, BACKLIGHT_FIRMWARE, BACKLIGHT_TYPE_MAX };
enum backlight_scale { BACKLIGHT_SCALE_UNKNOWN = 0, BACKLIGHT_SCALE_LINEAR, BACKLIGHT_SCALE_NON_LINEAR };
enum backlight_update_reason { BACKLIGHT_UPDATE_HOTKEY, BACKLIGHT_UPDATE_SYSFS };
#define BL_CORE_SUSPENDED (1 << 0)
#define BL_CORE_FBBLANK (1 << 1)
#define FB_BLANK_UNBLANK 0
#define FB_BLANK_POWERDOWN 4
struct backlight_device;
struct backlight_ops {
	unsigned int options;
	int (*update_status)(struct backlight_device *);
	int (*get_brightness)(struct backlight_device *);
	bool (*controls_device)(struct backlight_device *bd, struct device *dev);
};
#define BL_CORE_SUSPENDRESUME (1 << 0)
struct backlight_properties {
	int brightness, max_brightness, power, fb_blank;
	enum backlight_type type;
	unsigned int state;
	enum backlight_scale scale;
};
struct backlight_device {
	struct backlight_properties props;
	struct mutex update_lock, ops_lock;
	const struct backlight_ops *ops;
	struct device dev;
	bool fb_bl_on[32];
	int use_count;
};
struct backlight_device *backlight_device_register(const char *name, struct device *dev, void *devdata,
						   const struct backlight_ops *ops, const struct backlight_properties *props);
#define devm_backlight_device_register(d, n, p, dd, o, pr) backlight_device_register(n, p, dd, o, pr)
void backlight_device_unregister(struct backlight_device *bd);
#define backlight_device_get_by_name(name) ((struct backlight_device *)NULL)
#define put_device_backlight(bd) do { } while (0)
#define backlight_force_update(bd, reason) do { } while (0)
#define bl_get_data(bd) dev_get_drvdata(&(bd)->dev)
static inline int backlight_update_status(struct backlight_device *bd) { return bd->ops && bd->ops->update_status ? bd->ops->update_status(bd) : 0; }
static inline int backlight_get_brightness(const struct backlight_device *bd) { return bd->props.power == FB_BLANK_UNBLANK ? bd->props.brightness : 0; }
static inline bool backlight_is_blank(const struct backlight_device *bd) { return bd->props.power != FB_BLANK_UNBLANK || bd->props.brightness == 0; }
#define backlight_enable(bd) 0
#define backlight_disable(bd) 0
