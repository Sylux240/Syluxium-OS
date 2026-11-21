/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
#define orderly_poweroff(force) do { } while (0)
#define register_reboot_notifier(nb) 0
#define unregister_reboot_notifier(nb) 0
#define SYS_DOWN 1
#define SYS_RESTART SYS_DOWN
#define SYS_HALT 2
#define SYS_POWER_OFF 3
#define hw_protection_shutdown(reason, ms) do { } while (0)
