#ifndef _LKPI_LINUX_PM_H
#define _LKPI_LINUX_PM_H
#include <linux/kernel.h>
struct device;
struct dev_pm_ops {
    int (*suspend)(struct device* dev);
    int (*resume)(struct device* dev);
    int (*freeze)(struct device* dev);
    int (*thaw)(struct device* dev);
    int (*poweroff)(struct device* dev);
    int (*restore)(struct device* dev);
    int (*runtime_suspend)(struct device* dev);
    int (*runtime_resume)(struct device* dev);
};
#define SIMPLE_DEV_PM_OPS(name, s, r) const struct dev_pm_ops name __attribute__((unused)) = { .suspend = s, .resume = r, .freeze = s, .thaw = r, .poweroff = s, .restore = r }
#define DEFINE_SIMPLE_DEV_PM_OPS(name, s, r) SIMPLE_DEV_PM_OPS(name, s, r)
#define pm_sleep_ptr(p) (p)
#define pm_ptr(p) (p)
#define __maybe_unused_pm __attribute__((unused))
#endif
