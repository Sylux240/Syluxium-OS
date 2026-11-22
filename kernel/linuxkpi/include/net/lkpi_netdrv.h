/* Ethernet drivers imported from Linux 7.4-rc3 (igb, ...): the 7.x network
 * driver API on top of the r8169/e1000e/tg3 set (lkpi_phy.h, Linux 6.12).
 * Force-included for those drivers (Makefile). */
#pragma once
#define LKPI_V7_CYCLECOUNTER 1 /* linux/timecounter.h: 7.x signatures */
#include <net/lkpi_phy.h>
#include <linux/lkpi_v7.h>
#include <net/xdp_sock_drv.h>

/* AF_XDP zero-copy (Linux 7.x driver side): no XDP sockets in Syluxium, so
 * a driver never gets a pool; what its zero-copy paths touch still has to
 * exist. */
struct xsk_buff_pool {
	struct device *dev;
	struct xdp_desc *tx_descs;
};
static inline void xsk_buff_set_size(struct xdp_buff *xdp, u32 size) { }
static inline u32 xsk_tx_peek_release_desc_batch(struct xsk_buff_pool *pool, u32 max) { return 0; }

/* ---- NAPI / queues (7.x) ---- */
/* Persistent per-index NAPI config and the queue <-> NAPI map exist for
 * netlink introspection (netdev-genl), which Syluxium doesn't offer. */
#define netif_napi_add_config(dev, napi, poll, index) netif_napi_add(dev, napi, poll)
static inline void netif_napi_set_irq(struct napi_struct *napi, int irq) { }
enum netdev_queue_type { NETDEV_QUEUE_TYPE_RX, NETDEV_QUEUE_TYPE_TX };
static inline void netif_queue_set_napi(struct net_device *dev, unsigned int queue_index,
					enum netdev_queue_type type, struct napi_struct *napi) { }

/* ---- offloads ---- */
#ifndef NETIF_F_GSO_UDP_L4
#define NETIF_F_GSO_UDP_L4 BIT_ULL(54)
#endif
#ifndef SKB_GSO_UDP_L4
#define SKB_GSO_UDP_L4 (1 << 16)
#endif
/* Launch time (ETF qdisc): consumed by the driver; no such qdisc here. */
static inline void skb_txtime_consumed(struct sk_buff *skb) { skb->tstamp = 0; }

/* ---- address lists / bridge FDB ---- */
static inline int __dev_uc_unsync(struct net_device *dev, int (*unsync)(struct net_device *, const unsigned char *))
{
	return 0;
}
static inline int ndo_dflt_fdb_add(struct ndmsg *ndm, struct nlattr *tb[], struct net_device *dev,
				   const unsigned char *addr, u16 vid, u16 flags)
{
	return -EOPNOTSUPP;
}
