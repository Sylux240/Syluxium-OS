/* LinuxKPI: io_mapping - a write-combining window on device memory (GPU
 * apertures, VRAM BARs). The whole range is mapped once, at init. */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/io.h>
#include <linux/mm_types.h>

struct io_mapping {
	resource_size_t base;
	unsigned long size;
	pgprot_t prot;
	void __iomem *iomem;
};

static inline struct io_mapping *io_mapping_init_wc(struct io_mapping *iomap, resource_size_t base, unsigned long size)
{
	iomap->iomem = ioremap_wc(base, size);
	if (!iomap->iomem)
		return NULL;
	iomap->base = base;
	iomap->size = size;
	iomap->prot = pgprot_writecombine(PAGE_KERNEL);
	return iomap;
}

static inline void io_mapping_fini(struct io_mapping *mapping)
{
	iounmap(mapping->iomem);
}

static inline struct io_mapping *io_mapping_create_wc(resource_size_t base, unsigned long size)
{
	struct io_mapping *iomap = kmalloc(sizeof(*iomap), GFP_KERNEL);
	if (iomap && !io_mapping_init_wc(iomap, base, size)) {
		kfree(iomap);
		iomap = NULL;
	}
	return iomap;
}

static inline void io_mapping_free(struct io_mapping *iomap)
{
	io_mapping_fini(iomap);
	kfree(iomap);
}

static inline void __iomem *io_mapping_map_wc(struct io_mapping *mapping, unsigned long offset, unsigned long size)
{
	return (char __iomem *)mapping->iomem + offset;
}
#define io_mapping_map_atomic_wc(m, off) io_mapping_map_wc(m, off, PAGE_SIZE)
#define io_mapping_map_local_wc(m, off) io_mapping_map_wc(m, off, PAGE_SIZE)
#define io_mapping_unmap(v) do { (void)(v); } while (0)
#define io_mapping_unmap_atomic(v) do { (void)(v); } while (0)
#define io_mapping_unmap_local(v) do { (void)(v); } while (0)

/* i915: map part of an io_mapping into a user VMA. */
int io_mapping_map_user(struct io_mapping *iomap, struct vm_area_struct *vma, unsigned long addr,
			unsigned long pfn, unsigned long size);
