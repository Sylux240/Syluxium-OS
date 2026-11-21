/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: MMU notifiers (userptr / SVM): types only, never registered. */
struct mmu_notifier;
struct mmu_notifier_range { unsigned long start, end; int event; };
struct mmu_notifier_ops { void (*release)(struct mmu_notifier *, struct mm_struct *); };
struct mmu_notifier { const struct mmu_notifier_ops *ops; struct mm_struct *mm; };
struct mmu_interval_notifier;
struct mmu_interval_notifier_ops { bool (*invalidate)(struct mmu_interval_notifier *, const struct mmu_notifier_range *, unsigned long); };
struct mmu_interval_notifier { const struct mmu_interval_notifier_ops *ops; struct mm_struct *mm; unsigned long invalidate_seq; };
#define mmu_notifier_register(mn, mm) (-ENODEV)
#define mmu_notifier_unregister(mn, mm) do { } while (0)
#define mmu_notifier_put(mn) do { } while (0)
#define mmu_interval_notifier_insert(n, mm, start, len, ops) (-ENODEV)
#define mmu_interval_notifier_remove(n) do { } while (0)
#define mmu_interval_read_begin(n) 0UL
#define mmu_interval_read_retry(n, seq) false
#define mmu_interval_check_retry(n, seq) false
#define mmu_interval_set_seq(n, seq) do { } while (0)
#define mmu_notifier_range_blockable(r) true
