#ifndef _LKPI_LINUX_DMA_MAPPING_H
#define _LKPI_LINUX_DMA_MAPPING_H

#include <linux/device.h>
#include <linux/kernel.h>

LKPI_EXTERN_C_BEGIN

/* No IOMMU: bus addresses are physical addresses. Memory is cache-coherent
 * on x86, so the sync calls are barriers only. */
enum dma_data_direction {
    DMA_BIDIRECTIONAL = 0,
    DMA_TO_DEVICE = 1,
    DMA_FROM_DEVICE = 2,
    DMA_NONE = 3,
};

#define DMA_BIT_MASK(n) (((n) >= 64) ? ~0ULL : ((1ULL << ((n) & 63)) - 1))

void* dma_alloc_coherent(struct device* dev, size_t size, dma_addr_t* handle, gfp_t gfp);
#define dma_zalloc_coherent dma_alloc_coherent
#define dmam_alloc_coherent dma_alloc_coherent
void dma_free_coherent(struct device* dev, size_t size, void* cpu, dma_addr_t handle);
dma_addr_t dma_map_single(struct device* dev, void* ptr, size_t size, enum dma_data_direction dir);
#define dma_map_single_attrs(d, p, s, dir, a) dma_map_single(d, p, s, dir)
static inline void dma_unmap_single(struct device* dev, dma_addr_t a, size_t s, enum dma_data_direction d) {}
struct page;
dma_addr_t dma_map_page(struct device* dev, struct page* page, size_t offset, size_t size, enum dma_data_direction dir);
#define dma_map_page_attrs(d, p, o, s, dir, a) dma_map_page(d, p, o, s, dir)
#define dma_unmap_page_attrs(d, a, s, dir, at) dma_unmap_page(d, a, s, dir)
#define dma_map_resource(d, phys, s, dir, a) ((dma_addr_t)(phys))
#define dma_unmap_resource(d, a, s, dir, at) do { } while (0)
#define dma_unmap_single_attrs(d, a, s, dir, at) dma_unmap_single(d, a, s, dir)
#define dma_max_mapping_size(d) (~(size_t)0)
#define dma_addressing_limited(d) false
#define dma_get_required_mask(d) DMA_BIT_MASK(64)
#define dma_set_max_seg_size(d, s) 0
#define dma_get_max_seg_size(d) 0x7fffffffu
#define dma_set_seg_boundary(d, m) 0
#define dma_sync_single_range_for_cpu(d, a, o, s, dir) dma_sync_single_for_cpu(d, (a) + (o), s, dir)
#define dma_sync_single_range_for_device(d, a, o, s, dir) dma_sync_single_for_device(d, (a) + (o), s, dir)
static inline void dma_unmap_page(struct device* dev, dma_addr_t a, size_t s, enum dma_data_direction d) {}
static inline int dma_mapping_error(struct device* dev, dma_addr_t a) { return a == 0; }
static inline void dma_sync_single_for_cpu(struct device* d, dma_addr_t a, size_t s, enum dma_data_direction dir) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static inline void dma_sync_single_for_device(struct device* d, dma_addr_t a, size_t s, enum dma_data_direction dir) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
int dma_set_mask(struct device* dev, u64 mask);
int dma_set_coherent_mask(struct device* dev, u64 mask);
int dma_set_mask_and_coherent(struct device* dev, u64 mask);
#define dma_get_mask(d) (*(d)->dma_mask)

/* Pools of small coherent blocks. */
struct dma_pool;
struct dma_pool* dma_pool_create(const char* name, struct device* dev, size_t size, size_t align, size_t boundary);
void dma_pool_destroy(struct dma_pool* pool);
void* dma_pool_alloc(struct dma_pool* pool, gfp_t gfp, dma_addr_t* handle);
#define dma_pool_zalloc(p, g, h) dma_pool_alloc(p, (g) | 0x100u, h)
void dma_pool_free(struct dma_pool* pool, void* vaddr, dma_addr_t handle);

LKPI_EXTERN_C_END

#endif
