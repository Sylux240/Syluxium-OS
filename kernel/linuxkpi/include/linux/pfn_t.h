/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
typedef struct { u64 val; } pfn_t;
#define PFN_DEV (1ULL << 62)
#define PFN_MAP (1ULL << 61)
#define __pfn_to_pfn_t(pfn, flags) ((pfn_t){ (u64)(pfn) | (flags) })
#define pfn_t_to_pfn(p) ((unsigned long)((p).val & ~(PFN_DEV | PFN_MAP)))
