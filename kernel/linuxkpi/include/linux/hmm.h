/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
struct hmm_range { unsigned long start, end; unsigned long *hmm_pfns; unsigned long notifier_seq; void *dev_private_owner; };
#define hmm_range_fault(r) (-EFAULT)
