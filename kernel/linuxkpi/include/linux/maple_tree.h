/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: maple trees - the type only (nouveau's VM_BIND / uvmm is not built). */
struct maple_tree { spinlock_t ma_lock; void *ma_root; unsigned int ma_flags; };
#define MT_FLAGS_ALLOC_RANGE 0x01
#define MT_FLAGS_LOCK_EXTERN 0x300
