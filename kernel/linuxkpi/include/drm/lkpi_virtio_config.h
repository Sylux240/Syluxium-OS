/* LinuxKPI: the virtio core (virtio-pci, modern devices only) and the
 * virtio-gpu DRM driver Kconfig (included first by their sources). QEMU's
 * virtio-gpu with virgl/venus is the test bed for the GPU userspace path
 * (Mesa's device discovery, DRI3, GBM, dma-buf) that real GPUs share. */
#pragma once
#define CONFIG_VIRTIO 1
#define CONFIG_VIRTIO_ANCHOR 1
#define CONFIG_VIRTIO_PCI 1
#define CONFIG_VIRTIO_PCI_LIB 1
#define CONFIG_VIRTIO_DMA_SHARED_BUFFER 1
#define CONFIG_DRM_VIRTIO_GPU 1
#define CONFIG_DRM_VIRTIO_GPU_KMS 1

#include <linux/lkpi_all.h>
#include <linux/pci.h>
#include <linux/of.h>

/* CPU masks: interrupts aren't spread over CPUs by affinity here (one
 * vector per queue still works; the masks are never looked at). */
typedef struct cpumask { unsigned long bits[1]; } cpumask_t;
typedef struct cpumask cpumask_var_t[1];
static inline bool alloc_cpumask_var(cpumask_var_t *m, gfp_t flags) { return true; }
static inline bool zalloc_cpumask_var(cpumask_var_t *m, gfp_t flags) { (*m)->bits[0] = 0; return true; }
static inline void free_cpumask_var(cpumask_var_t m) { }
static inline void cpumask_copy(struct cpumask *dst, const struct cpumask *src) { *dst = *src; }
static inline int irq_update_affinity_hint(unsigned int irq, const struct cpumask *m) { return 0; }
static inline int irq_set_affinity_and_hint(unsigned int irq, const struct cpumask *m) { return 0; }

struct irq_affinity {
	unsigned int pre_vectors;
	unsigned int post_vectors;
	unsigned int nr_sets;
	unsigned int set_size[4];
	void (*calc_sets)(struct irq_affinity *, unsigned int nvecs);
	void *priv;
};
#define PCI_IRQ_AFFINITY 0x80
#define pci_alloc_irq_vectors_affinity(dev, min, max, flags, desc) \
	pci_alloc_irq_vectors(dev, min, max, (flags) & ~PCI_IRQ_AFFINITY)
static inline const struct cpumask *pci_irq_get_affinity(struct pci_dev *dev, int nr) { return NULL; }

#ifndef SMP_CACHE_BYTES
#define SMP_CACHE_BYTES 64
#endif

/* No device tree on PCs. */
static inline struct device_node *dev_of_node(struct device *dev) { return NULL; }
static inline int of_get_available_child_count(const struct device_node *np) { return 0; }
static inline struct device_node *of_get_next_available_child(const struct device_node *node,
							       struct device_node *prev) { return NULL; }
static inline int of_device_is_compatible(const struct device_node *np, const char *compat) { return 0; }

/* No SR-IOV. */
static inline void pci_disable_sriov(struct pci_dev *dev) { }
static inline int pci_enable_sriov(struct pci_dev *dev, int nr) { return -ENODEV; }
static inline int pci_vfs_assigned(struct pci_dev *dev) { return 0; }
#define pci_iov_get_pf_drvdata(dev, drv) ERR_PTR(-ENODEV)

/* Rings: physically contiguous pages from the direct map. */
static inline void *alloc_pages_exact(size_t size, gfp_t gfp)
{
	return (void *)__get_free_pages(gfp, get_order(size));
}
static inline void free_pages_exact(void *virt, size_t size)
{
	free_pages((unsigned long)virt, get_order(size));
}
#define dma_need_sync(dev, addr) false
#ifndef PAGE_ALIGNED
#define PAGE_ALIGNED(addr) (((unsigned long)(addr) & (PAGE_SIZE - 1)) == 0)
#endif
/* x86 DMA is cache-coherent. */
#define dma_sync_sgtable_for_device(dev, sgt, dir) do { } while (0)
#define dma_sync_sgtable_for_cpu(dev, sgt, dir) do { } while (0)
static inline bool pci_is_vga(struct pci_dev *pdev)
{
	return (pdev->class >> 8) == 0x0300 || (pdev->class >> 8) == 0x0001; /* VGA, pre-2.0 VGA */
}
