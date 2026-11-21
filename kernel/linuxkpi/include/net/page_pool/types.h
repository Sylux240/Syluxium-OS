/* LinuxKPI: page pools. No recycling here: every buffer is a page of its
 * own (frag offset 0), mapped for DMA when the pool asks for it, and given
 * back to the page allocator when returned. */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/dma-mapping.h>

#define PP_FLAG_DMA_MAP 0x1
#define PP_FLAG_DMA_SYNC_DEV 0x2
#define PP_FLAG_SYSTEM_POOL 0x4
#define PP_FLAG_ALLOW_UNREADABLE_NETMEM 0x8

struct napi_struct;
struct net_device;

struct page_pool_params {
    unsigned int order;
    unsigned int flags;
    unsigned int pool_size;
    int nid;
    struct device *dev;
    struct napi_struct *napi;
    enum dma_data_direction dma_dir;
    unsigned int max_len;
    unsigned int offset;
    struct net_device *netdev;
    unsigned int queue_idx;
};

struct page_pool {
    struct page_pool_params p;
};

struct page_pool_stats {
    u64 unused;
};

struct page_pool *page_pool_create(const struct page_pool_params *params);
void page_pool_destroy(struct page_pool *pool);
struct page *page_pool_alloc_frag(struct page_pool *pool, unsigned int *offset, unsigned int size, gfp_t gfp);
void page_pool_put_full_page(struct page_pool *pool, struct page *page, bool allow_direct);
