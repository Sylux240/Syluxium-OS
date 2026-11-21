/* LinuxKPI: page pool helpers (see types.h). */
#pragma once
#include <net/page_pool/types.h>
#include <linux/skbuff.h>

static inline struct page *page_pool_dev_alloc_frag(struct page_pool *pool, unsigned int *offset, unsigned int size)
{
    return page_pool_alloc_frag(pool, offset, size, GFP_ATOMIC);
}
static inline struct page *page_pool_dev_alloc_pages(struct page_pool *pool)
{
    unsigned int offset;
    return page_pool_alloc_frag(pool, &offset, PAGE_SIZE, GFP_ATOMIC);
}
static inline dma_addr_t page_pool_get_dma_addr(const struct page *page) { return page->dma_addr; }
static inline enum dma_data_direction page_pool_get_dma_dir(const struct page_pool *pool) { return pool->p.dma_dir; }
static inline void page_pool_recycle_direct(struct page_pool *pool, struct page *page)
{
    page_pool_put_full_page(pool, page, true);
}
static inline void skb_mark_for_recycle(struct sk_buff *skb) { }

/* Statistics: none kept. */
static inline bool page_pool_get_stats(const struct page_pool *pool, struct page_pool_stats *stats) { return false; }
static inline int page_pool_ethtool_stats_get_count(void) { return 0; }
static inline u8 *page_pool_ethtool_stats_get_strings(u8 *data) { return data; }
static inline u64 *page_pool_ethtool_stats_get(u64 *data, const void *stats) { return data; }
