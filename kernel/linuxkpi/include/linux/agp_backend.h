/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: AGP - none (PCI/PCIe GPUs only). */
struct agp_bridge_data;
struct agp_memory { struct page **pages; size_t page_count; int type; bool is_bound; bool is_flushed; off_t pg_start; };
struct agp_version { u16 major, minor; };
struct agp_kern_info { struct agp_version version; struct pci_dev *device; int chipset; unsigned long mode; unsigned long aper_base; size_t aper_size; int max_memory; int current_memory; bool cant_use_aperture; unsigned long page_mask; const struct vm_operations_struct *vm_ops; };
#define agp_backend_acquire(pdev) ((struct agp_bridge_data *)NULL)
#define agp_backend_release(b) do { } while (0)
#define agp_copy_info(b, info) (-ENODEV)
#define agp_enable(b, mode) do { } while (0)
#define agp_free_memory(m) do { } while (0)
#define agp_allocate_memory(b, n, t) ((struct agp_memory *)NULL)
#define agp_bind_memory(m, off) (-ENODEV)
#define agp_unbind_memory(m) (-ENODEV)
#define AGP_USER_MEMORY 1
#define AGP_USER_CACHED_MEMORY 2
