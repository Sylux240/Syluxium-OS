#pragma once
#include <linux/types.h>
static inline void kcov_remote_start_common(u64 id) {}
static inline void kcov_remote_stop(void) {}
static inline void kcov_remote_start_usb(u64 id) {}
