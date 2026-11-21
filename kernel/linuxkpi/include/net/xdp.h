/* LinuxKPI: XDP (eBPF programs run on received packets in the driver).
 * Syluxium has no BPF: no program is ever attached, so drivers take their
 * normal path (XDP_PASS). The types are Linux's (net/xdp.h, uapi bpf.h) so
 * drivers with XDP support (igc) compile unchanged. */
#pragma once

#include <linux/lkpi_all.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>

enum xdp_action { XDP_ABORTED = 0, XDP_DROP, XDP_PASS, XDP_TX, XDP_REDIRECT };

struct xdp_md {
    __u32 data, data_end, data_meta, ingress_ifindex, rx_queue_index, egress_ifindex;
};

enum xdp_rss_hash_type {
    XDP_RSS_L3_IPV4 = BIT(0),
    XDP_RSS_L3_IPV6 = BIT(1),
    XDP_RSS_L3_DYNHDR = BIT(2),
    XDP_RSS_L4 = BIT(3),
    XDP_RSS_L4_TCP = BIT(4),
    XDP_RSS_L4_UDP = BIT(5),
    XDP_RSS_L4_SCTP = BIT(6),
    XDP_RSS_L4_IPSEC = BIT(7),
    XDP_RSS_L4_ICMP = BIT(8),
    XDP_RSS_TYPE_NONE = 0,
    XDP_RSS_TYPE_L2 = XDP_RSS_TYPE_NONE,
    XDP_RSS_TYPE_L3_IPV4 = XDP_RSS_L3_IPV4,
    XDP_RSS_TYPE_L3_IPV6 = XDP_RSS_L3_IPV6,
    XDP_RSS_TYPE_L3_IPV4_OPT = XDP_RSS_L3_IPV4 | XDP_RSS_L3_DYNHDR,
    XDP_RSS_TYPE_L3_IPV6_EX = XDP_RSS_L3_IPV6 | XDP_RSS_L3_DYNHDR,
    XDP_RSS_TYPE_L4_ANY = XDP_RSS_L4,
    XDP_RSS_TYPE_L4_IPV4_TCP = XDP_RSS_L3_IPV4 | XDP_RSS_L4 | XDP_RSS_L4_TCP,
    XDP_RSS_TYPE_L4_IPV4_UDP = XDP_RSS_L3_IPV4 | XDP_RSS_L4 | XDP_RSS_L4_UDP,
    XDP_RSS_TYPE_L4_IPV4_SCTP = XDP_RSS_L3_IPV4 | XDP_RSS_L4 | XDP_RSS_L4_SCTP,
    XDP_RSS_TYPE_L4_IPV4_IPSEC = XDP_RSS_L3_IPV4 | XDP_RSS_L4 | XDP_RSS_L4_IPSEC,
    XDP_RSS_TYPE_L4_IPV4_ICMP = XDP_RSS_L3_IPV4 | XDP_RSS_L4 | XDP_RSS_L4_ICMP,
    XDP_RSS_TYPE_L4_IPV6_TCP = XDP_RSS_L3_IPV6 | XDP_RSS_L4 | XDP_RSS_L4_TCP,
    XDP_RSS_TYPE_L4_IPV6_UDP = XDP_RSS_L3_IPV6 | XDP_RSS_L4 | XDP_RSS_L4_UDP,
    XDP_RSS_TYPE_L4_IPV6_SCTP = XDP_RSS_L3_IPV6 | XDP_RSS_L4 | XDP_RSS_L4_SCTP,
    XDP_RSS_TYPE_L4_IPV6_IPSEC = XDP_RSS_L3_IPV6 | XDP_RSS_L4 | XDP_RSS_L4_IPSEC,
    XDP_RSS_TYPE_L4_IPV6_ICMP = XDP_RSS_L3_IPV6 | XDP_RSS_L4 | XDP_RSS_L4_ICMP,
    XDP_RSS_TYPE_L4_IPV6_TCP_EX = XDP_RSS_TYPE_L4_IPV6_TCP | XDP_RSS_L3_DYNHDR,
    XDP_RSS_TYPE_L4_IPV6_UDP_EX = XDP_RSS_TYPE_L4_IPV6_UDP | XDP_RSS_L3_DYNHDR,
    XDP_RSS_TYPE_L4_IPV6_SCTP_EX = XDP_RSS_TYPE_L4_IPV6_SCTP | XDP_RSS_L3_DYNHDR,
};

struct xdp_metadata_ops {
    int (*xmo_rx_timestamp)(const struct xdp_md *ctx, u64 *timestamp);
    int (*xmo_rx_hash)(const struct xdp_md *ctx, u32 *hash, enum xdp_rss_hash_type *rss_type);
    int (*xmo_rx_vlan_tag)(const struct xdp_md *ctx, __be16 *vlan_proto, u16 *vlan_tci);
};

enum xdp_mem_type { MEM_TYPE_PAGE_SHARED = 0, MEM_TYPE_PAGE_ORDER0, MEM_TYPE_PAGE_POOL, MEM_TYPE_XSK_BUFF_POOL, MEM_TYPE_MAX };

struct xdp_mem_info {
    u32 type;
    u32 id;
};

struct xdp_rxq_info {
    struct net_device *dev;
    u32 queue_index;
    u32 reg_state;
    struct xdp_mem_info mem;
    unsigned int napi_id;
    u32 frag_size;
};

struct xdp_txq_info {
    struct net_device *dev;
};

struct xdp_buff {
    void *data;
    void *data_end;
    void *data_meta;
    void *data_hard_start;
    struct xdp_rxq_info *rxq;
    struct xdp_txq_info *txq;
    u32 frame_sz;
    u32 flags;
};

struct xdp_frame {
    void *data;
    u16 len;
    u16 headroom;
    u32 metasize;
    struct xdp_mem_info mem;
    struct net_device *dev_rx;
    u32 frame_sz;
    u32 flags;
};

static inline void xdp_init_buff(struct xdp_buff *xdp, u32 frame_sz, struct xdp_rxq_info *rxq)
{
    xdp->frame_sz = frame_sz;
    xdp->rxq = rxq;
    xdp->flags = 0;
}

static inline void xdp_prepare_buff(struct xdp_buff *xdp, unsigned char *hard_start, int headroom, int data_len,
                                    const bool meta_valid)
{
    unsigned char *data = hard_start + headroom;
    xdp->data_hard_start = hard_start;
    xdp->data = data;
    xdp->data_end = data + data_len;
    xdp->data_meta = meta_valid ? data : data + 1;
}

static inline struct xdp_frame *xdp_convert_buff_to_frame(struct xdp_buff *xdp) { return NULL; }
static inline void xdp_return_frame(struct xdp_frame *xdpf) { }
static inline void xdp_return_frame_rx_napi(struct xdp_frame *xdpf) { }
static inline bool xdp_buff_has_frags(const struct xdp_buff *xdp) { return false; }
static inline bool xdp_frame_has_frags(const struct xdp_frame *f) { return false; }
static inline struct skb_shared_info *xdp_get_shared_info_from_frame(const struct xdp_frame *f) { return NULL; }
static inline struct skb_shared_info *xdp_get_shared_info_from_buff(const struct xdp_buff *xdp) { return NULL; }

#define XDP_RXQ_INFO_REG 1
static inline int __xdp_rxq_info_reg(struct xdp_rxq_info *rxq, struct net_device *dev, u32 queue_index,
                                     unsigned int napi_id, u32 frag_size)
{
    rxq->dev = dev;
    rxq->queue_index = queue_index;
    rxq->napi_id = napi_id;
    rxq->frag_size = frag_size;
    rxq->reg_state = XDP_RXQ_INFO_REG;
    return 0;
}
#define xdp_rxq_info_reg(rxq, dev, qi, napi_id) __xdp_rxq_info_reg(rxq, dev, qi, napi_id, 0)
static inline void xdp_rxq_info_unreg(struct xdp_rxq_info *rxq) { rxq->reg_state = 0; }
static inline bool xdp_rxq_info_is_reg(struct xdp_rxq_info *rxq) { return rxq->reg_state == XDP_RXQ_INFO_REG; }
static inline int xdp_rxq_info_reg_mem_model(struct xdp_rxq_info *rxq, enum xdp_mem_type type, void *allocator)
{
    rxq->mem.type = type;
    return 0;
}
static inline void xdp_rxq_info_unreg_mem_model(struct xdp_rxq_info *rxq) { }

/* eBPF programs: none can be loaded. */
struct bpf_prog;
static inline u32 bpf_prog_run_xdp(const struct bpf_prog *prog, struct xdp_buff *xdp) { return XDP_PASS; }
static inline void bpf_prog_put(struct bpf_prog *prog) { }
static inline void bpf_warn_invalid_xdp_action(struct net_device *dev, struct bpf_prog *prog, u32 act) { }
static inline int xdp_do_redirect(struct net_device *dev, struct xdp_buff *xdp, struct bpf_prog *prog) { return -EOPNOTSUPP; }
static inline void xdp_do_flush(void) { }
#define xdp_do_flush_map xdp_do_flush
static inline struct sk_buff *xdp_build_skb_from_frame(struct xdp_frame *f, struct net_device *dev) { return NULL; }

enum bpf_netdev_command {
    XDP_SETUP_PROG,
    XDP_SETUP_PROG_HW,
    BPF_OFFLOAD_MAP_ALLOC,
    BPF_OFFLOAD_MAP_FREE,
    XDP_SETUP_XSK_POOL,
};
struct xsk_buff_pool;
struct netlink_ext_ack;
struct netdev_bpf {
    enum bpf_netdev_command command;
    union {
        struct {
            u32 flags;
            struct bpf_prog *prog;
            struct netlink_ext_ack *extack;
        };
        struct {
            struct xsk_buff_pool *pool;
            u16 queue_id;
        } xsk;
    };
};

#define XDP_XMIT_FLUSH (1U << 0)
#define XDP_XMIT_FLAGS_MASK XDP_XMIT_FLUSH
#define XDP_WAKEUP_RX (1 << 0)
#define XDP_WAKEUP_TX (1 << 1)

#define NETDEV_XDP_ACT_BASIC 1
#define NETDEV_XDP_ACT_REDIRECT 2
#define NETDEV_XDP_ACT_NDO_XMIT 4
#define NETDEV_XDP_ACT_XSK_ZEROCOPY 8
#define NETDEV_XDP_ACT_HW_OFFLOAD 16
#define NETDEV_XDP_ACT_RX_SG 32
#define NETDEV_XDP_ACT_NDO_XMIT_SG 64
#define NETDEV_XDP_ACT_XSK 128

struct xsk_tx_metadata_compl {
    __u64 *tx_timestamp;
};
#define XDP_PACKET_HEADROOM 256
#define XSK_CHECK_PRIV_TYPE(t) do { } while (0)
static inline unsigned int xdp_get_frame_len(const struct xdp_frame *xdpf) { return xdpf->len; }
static inline void xdp_buff_clear_frags_flag(struct xdp_buff *xdp) { }
static inline void xdp_features_set_redirect_target(struct net_device *dev, bool support_sg) { }
static inline void xdp_features_clear_redirect_target(struct net_device *dev) { }
