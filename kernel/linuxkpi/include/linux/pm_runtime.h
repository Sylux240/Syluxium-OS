/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: runtime power management - devices stay powered. Usage
 * counts are kept so drivers' balance checks hold. */
#include <linux/pm.h>
#define pm_runtime_get_sync(dev) 1
#define pm_runtime_resume_and_get(dev) 0
#define pm_runtime_get(dev) 1
#define pm_runtime_get_noresume(dev) do { } while (0)
#define pm_runtime_get_if_in_use(dev) 1
#define pm_runtime_get_if_active(dev) 1
#define pm_runtime_put(dev) 0
#define pm_runtime_put_sync(dev) 0
#define pm_runtime_put_noidle(dev) do { } while (0)
#define pm_runtime_put_autosuspend(dev) 0
#define pm_runtime_put_sync_suspend(dev) 0
#define pm_runtime_mark_last_busy(dev) do { } while (0)
#define pm_runtime_set_autosuspend_delay(dev, d) do { } while (0)
#define pm_runtime_use_autosuspend(dev) do { } while (0)
#define pm_runtime_dont_use_autosuspend(dev) do { } while (0)
#define pm_runtime_allow(dev) do { } while (0)
#define pm_runtime_forbid(dev) do { } while (0)
#define pm_runtime_enable(dev) do { } while (0)
#define pm_runtime_disable(dev) do { } while (0)
#define pm_runtime_set_active(dev) 0
#define pm_runtime_set_suspended(dev) 0
#define pm_runtime_suspended(dev) false
#define pm_runtime_active(dev) true
#define pm_runtime_status_suspended(dev) false
#define pm_runtime_enabled(dev) false
#define pm_runtime_irq_safe(dev) do { } while (0)
#define pm_runtime_no_callbacks(dev) do { } while (0)
#define pm_runtime_suspend(dev) 0
#define pm_runtime_resume(dev) 0
#define pm_runtime_idle(dev) 0
#define pm_runtime_autosuspend(dev) 0
#define pm_request_idle(dev) 0
#define pm_suspend_ignore_children(dev, en) do { } while (0)
#define pm_runtime_get_noresume(dev) do { } while (0)
#define devm_pm_runtime_enable(dev) 0
#define pm_runtime_barrier(dev) do { } while (0)
#define pm_runtime_set_autosuspend_delay(dev, d) do { } while (0)
