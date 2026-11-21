/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
#define console_lock() do { } while (0)
#define console_unlock() do { } while (0)
#define console_trylock() 1
#define console_conditional_schedule() do { } while (0)
#define is_console_locked() 1
