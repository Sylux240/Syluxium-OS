/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
struct sysrq_key_op { void (*handler)(u8); const char *help_msg, *action_msg; int enable_mask; };
#define register_sysrq_key(k, op) 0
#define unregister_sysrq_key(k, op) 0
