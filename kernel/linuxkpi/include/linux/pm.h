#ifndef _LKPI_LINUX_PM_H
#define _LKPI_LINUX_PM_H
/* LinuxKPI: device power management callbacks. Syluxium doesn't suspend
 * Linux devices yet; drivers fill these in and nothing calls them. */
#include <linux/kernel.h>
struct device;
typedef struct pm_message { int event; } pm_message_t;
#define PM_EVENT_ON 0x0000
#define PM_EVENT_FREEZE 0x0001
#define PM_EVENT_SUSPEND 0x0002
#define PM_EVENT_HIBERNATE 0x0004
#define PM_EVENT_QUIESCE 0x0008
#define PM_EVENT_RESUME 0x0010
#define PM_EVENT_PRETHAW PM_EVENT_QUIESCE
#define PM_EVENT_AUTO 0x0400
#define PMSG_SUSPEND ((pm_message_t){ .event = PM_EVENT_SUSPEND })
#define PMSG_FREEZE ((pm_message_t){ .event = PM_EVENT_FREEZE })
#define PMSG_HIBERNATE ((pm_message_t){ .event = PM_EVENT_HIBERNATE })
#define PMSG_QUIESCE ((pm_message_t){ .event = PM_EVENT_QUIESCE })
#define PMSG_ON ((pm_message_t){ .event = PM_EVENT_ON })
#define PMSG_RESUME ((pm_message_t){ .event = PM_EVENT_RESUME })
struct dev_pm_ops {
    int (*prepare)(struct device *dev);
    void (*complete)(struct device *dev);
    int (*suspend)(struct device *dev);
    int (*resume)(struct device *dev);
    int (*freeze)(struct device *dev);
    int (*thaw)(struct device *dev);
    int (*poweroff)(struct device *dev);
    int (*restore)(struct device *dev);
    int (*suspend_late)(struct device *dev);
    int (*resume_early)(struct device *dev);
    int (*freeze_late)(struct device *dev);
    int (*thaw_early)(struct device *dev);
    int (*poweroff_late)(struct device *dev);
    int (*restore_early)(struct device *dev);
    int (*suspend_noirq)(struct device *dev);
    int (*resume_noirq)(struct device *dev);
    int (*freeze_noirq)(struct device *dev);
    int (*thaw_noirq)(struct device *dev);
    int (*poweroff_noirq)(struct device *dev);
    int (*restore_noirq)(struct device *dev);
    int (*runtime_suspend)(struct device *dev);
    int (*runtime_resume)(struct device *dev);
    int (*runtime_idle)(struct device *dev);
};
/* CONFIG_PM / CONFIG_PM_SLEEP are off: the SET_* forms vanish (drivers
 * compile their callbacks out), the plain forms keep the pointers. */
#define SYSTEM_SLEEP_PM_OPS(s, r) .suspend = s, .resume = r, .freeze = s, .thaw = r, .poweroff = s, .restore = r,
#define LATE_SYSTEM_SLEEP_PM_OPS(s, r) .suspend_late = s, .resume_early = r, .freeze_late = s, .thaw_early = r, .poweroff_late = s, .restore_early = r,
#define NOIRQ_SYSTEM_SLEEP_PM_OPS(s, r) .suspend_noirq = s, .resume_noirq = r, .freeze_noirq = s, .thaw_noirq = r, .poweroff_noirq = s, .restore_noirq = r,
#define RUNTIME_PM_OPS(s, r, i) .runtime_suspend = s, .runtime_resume = r, .runtime_idle = i,
#define SET_SYSTEM_SLEEP_PM_OPS(s, r)
#define SET_LATE_SYSTEM_SLEEP_PM_OPS(s, r)
#define SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(s, r)
#define SET_RUNTIME_PM_OPS(s, r, i)
#define SIMPLE_DEV_PM_OPS(name, s, r) const struct dev_pm_ops name __attribute__((unused)) = { SYSTEM_SLEEP_PM_OPS(s, r) }
#define DEFINE_SIMPLE_DEV_PM_OPS(name, s, r) SIMPLE_DEV_PM_OPS(name, s, r)
#define DEFINE_RUNTIME_DEV_PM_OPS(name, s, r, i) const struct dev_pm_ops name __attribute__((unused)) = { SYSTEM_SLEEP_PM_OPS(s, r) RUNTIME_PM_OPS(s, r, i) }
#define UNIVERSAL_DEV_PM_OPS(name, s, r, i) DEFINE_RUNTIME_DEV_PM_OPS(name, s, r, i)
#define _DEFINE_DEV_PM_OPS(name, s, r, rs, rr, ri) const struct dev_pm_ops name __attribute__((unused)) = { SYSTEM_SLEEP_PM_OPS(s, r) RUNTIME_PM_OPS(rs, rr, ri) }
#define pm_sleep_ptr(p) (p)
#define pm_ptr(p) (p)
#define __maybe_unused_pm __attribute__((unused))
#define pm_suspend_via_firmware() false
#define pm_suspend_no_platform() false
#define pm_resume_via_firmware() false
#define pm_suspend_target_state 0
#define PM_SUSPEND_ON 0
#define PM_SUSPEND_TO_IDLE 1
#define PM_SUSPEND_MEM 3
#define device_init_wakeup(dev, e) 0
#define dev_pm_set_driver_flags(dev, f) do { } while (0)
#define DPM_FLAG_NO_DIRECT_COMPLETE 1
#define DPM_FLAG_SMART_PREPARE 2
#define DPM_FLAG_SMART_SUSPEND 4
#define DPM_FLAG_MAY_SKIP_RESUME 8
#endif
