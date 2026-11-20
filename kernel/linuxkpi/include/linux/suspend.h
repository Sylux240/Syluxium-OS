/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/pm.h>
#define pm_suspend_via_s2idle() false
#define register_pm_notifier(nb) 0
#define unregister_pm_notifier(nb) 0
#define PM_HIBERNATION_PREPARE 1
#define PM_POST_HIBERNATION 2
#define PM_SUSPEND_PREPARE 3
#define PM_POST_SUSPEND 4
#define PM_RESTORE_PREPARE 5
#define PM_POST_RESTORE 6
#define system_entering_hibernation() false
