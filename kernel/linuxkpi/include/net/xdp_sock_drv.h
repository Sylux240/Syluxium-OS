/* LinuxKPI: AF_XDP (user-space packet buffers mapped into the driver).
 * No AF_XDP sockets exist here, so no queue ever has a buffer pool and the
 * drivers' zero-copy paths stay unused. */
#pragma once

#include <net/xdp.h>

struct xdp_desc {
    u64 addr;
    u32 len;
    u32 options;
};

struct xsk_tx_metadata;
struct xsk_tx_metadata_ops {
    void (*tmo_request_timestamp)(void *priv);
    u64 (*tmo_fill_timestamp)(void *priv);
    void (*tmo_request_checksum)(u16 csum_start, u16 csum_offset, void *priv);
};

static inline struct xsk_buff_pool *xsk_get_pool_from_qid(struct net_device *dev, u16 queue_id) { return NULL; }
static inline int xsk_pool_dma_map(struct xsk_buff_pool *pool, struct device *dev, unsigned long attrs)
{
    return -EOPNOTSUPP;
}
static inline void xsk_pool_dma_unmap(struct xsk_buff_pool *pool, unsigned long attrs) { }
static inline u32 xsk_pool_get_rx_frame_size(struct xsk_buff_pool *pool) { return 0; }
static inline void xsk_pool_set_rxq_info(struct xsk_buff_pool *pool, struct xdp_rxq_info *rxq) { }
static inline struct xdp_buff *xsk_buff_alloc(struct xsk_buff_pool *pool) { return NULL; }
static inline u32 xsk_buff_alloc_batch(struct xsk_buff_pool *pool, struct xdp_buff **xdp, u32 max) { return 0; }
static inline void xsk_buff_free(struct xdp_buff *xdp) { }
static inline dma_addr_t xsk_buff_xdp_get_dma(struct xdp_buff *xdp) { return 0; }
static inline dma_addr_t xsk_buff_raw_get_dma(struct xsk_buff_pool *pool, u64 addr) { return 0; }
static inline void *xsk_buff_raw_get_data(struct xsk_buff_pool *pool, u64 addr) { return NULL; }
static inline void xsk_buff_raw_dma_sync_for_device(struct xsk_buff_pool *pool, dma_addr_t dma, size_t size) { }
static inline void xsk_buff_dma_sync_for_cpu(struct xdp_buff *xdp) { }
static inline struct xsk_tx_metadata *xsk_buff_get_metadata(struct xsk_buff_pool *pool, u64 addr) { return NULL; }
static inline bool xsk_tx_peek_desc(struct xsk_buff_pool *pool, struct xdp_desc *desc) { return false; }
static inline void xsk_tx_release(struct xsk_buff_pool *pool) { }
static inline void xsk_tx_completed(struct xsk_buff_pool *pool, u32 nb_entries) { }
static inline bool xsk_uses_need_wakeup(struct xsk_buff_pool *pool) { return false; }
static inline bool xp_tx_metadata_enabled(const struct xsk_buff_pool *pool) { return false; }
static inline void xsk_set_tx_need_wakeup(struct xsk_buff_pool *pool) { }
static inline void xsk_set_rx_need_wakeup(struct xsk_buff_pool *pool) { }
static inline void xsk_clear_tx_need_wakeup(struct xsk_buff_pool *pool) { }
static inline void xsk_clear_rx_need_wakeup(struct xsk_buff_pool *pool) { }
static inline void xsk_tx_metadata_request(const struct xsk_tx_metadata *meta,
                                           const struct xsk_tx_metadata_ops *ops, void *priv)
{
}
static inline void xsk_tx_metadata_to_compl(struct xsk_tx_metadata *meta, struct xsk_tx_metadata_compl *compl) { }
static inline void xsk_tx_metadata_complete(struct xsk_tx_metadata_compl *compl,
                                            const struct xsk_tx_metadata_ops *ops, void *priv)
{
}
