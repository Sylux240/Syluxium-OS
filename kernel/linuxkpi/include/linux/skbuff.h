/* LinuxKPI: socket buffers.
 *
 * Syluxium's sk_buffs are always linear: the packet lies in one buffer
 * (head .. data .. tail .. end) and data_len is 0. skb_add_rx_frag() copies
 * the fragment into the linear part; skb_clone() makes a full copy (so a
 * clone is never "cloned" and always writable). The shared info lives in
 * the sk_buff itself, with no fragments. The field and function names and
 * semantics are Linux's, so the wireless stack and drivers run unchanged. */
#ifndef _LKPI_LINUX_SKBUFF_H
#define _LKPI_LINUX_SKBUFF_H

#include <linux/lkpi_page.h>
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/rbtree_types.h>

LKPI_EXTERN_C_BEGIN

struct net_device;
struct napi_struct;
struct sock;
struct page;
struct sk_buff_head;

#define CHECKSUM_NONE 0
#define CHECKSUM_UNNECESSARY 1
#define CHECKSUM_COMPLETE 2
#define CHECKSUM_PARTIAL 3
#define NET_SKB_PAD 64
#define NET_IP_ALIGN 2
#define SKB_DATA_ALIGN(x) (((x) + 63) & ~63)
#define SKB_WITH_OVERHEAD(x) ((x) - 384)
#define SKB_TRUESIZE(x) ((x) + 512)
#define MAX_SKB_FRAGS 17

#define PACKET_HOST 0
#define PACKET_BROADCAST 1
#define PACKET_MULTICAST 2
#define PACKET_OTHERHOST 3
#define PACKET_OUTGOING 4
#define PACKET_LOOPBACK 5

typedef struct skb_frag {
    struct page* netmem;
    unsigned int len;
    unsigned int offset;
} skb_frag_t;

struct skb_shared_hwtstamps {
    union {
        ktime_t hwtstamp;
        void* netdev_data;
    };
};

enum {
    SKBTX_HW_TSTAMP = 1 << 0,
    SKBTX_SW_TSTAMP = 1 << 1,
    SKBTX_IN_PROGRESS = 1 << 2,
    SKBTX_WIFI_STATUS = 1 << 4,
    SKBTX_SCHED_TSTAMP = 1 << 6,
};
#define SKBTX_ANY_TSTAMP (SKBTX_HW_TSTAMP | SKBTX_SW_TSTAMP | SKBTX_SCHED_TSTAMP)

enum {
    SKB_GSO_TCPV4 = 1 << 0,
    SKB_GSO_DODGY = 1 << 1,
    SKB_GSO_TCP_ECN = 1 << 2,
    SKB_GSO_TCPV6 = 1 << 4,
};

struct skb_shared_info {
    u8 flags;
    u8 meta_len;
    u8 nr_frags;
    u8 tx_flags;
    unsigned short gso_size;
    unsigned short gso_segs;
    struct sk_buff* frag_list;
    struct skb_shared_hwtstamps hwtstamps;
    unsigned int gso_type;
    u32 tskey;
    atomic_t dataref;
    void* destructor_arg;
    skb_frag_t frags[MAX_SKB_FRAGS];
};

/* Drop reasons (only the generic ones; reasons are not reported). */
enum skb_drop_reason {
    SKB_NOT_DROPPED_YET = 0,
    SKB_CONSUMED,
    SKB_DROP_REASON_NOT_SPECIFIED,
    SKB_DROP_REASON_NOMEM,
    SKB_DROP_REASON_PKT_TOO_SMALL,
    SKB_DROP_REASON_DEV_READY,
    SKB_DROP_REASON_FULL_RING,
    SKB_DROP_REASON_NOCARRIER,
    SKB_DROP_REASON_MAX,
    SKB_DROP_REASON_SUBSYS_MASK = 0xffff0000,
};
#define SKB_DROP_REASON_SUBSYS_SHIFT 16
#define SKB_DR_INIT(name, reason) enum skb_drop_reason name = SKB_DROP_REASON_##reason
#define SKB_DR(name) SKB_DR_INIT(name, NOT_SPECIFIED)
#define SKB_DR_SET(name, reason) (name = SKB_DROP_REASON_##reason)
#define SKB_DR_OR(name, reason) do { if (name == SKB_DROP_REASON_NOT_SPECIFIED || name == SKB_NOT_DROPPED_YET) SKB_DR_SET(name, reason); } while (0)

struct sk_buff {
    union {
        struct {
            struct sk_buff* next;
            struct sk_buff* prev;
            union {
                struct net_device* dev;
                unsigned long dev_scratch;
            };
        };
        struct rb_node rbnode;
        struct list_head list;
    };
    struct sock* sk;
    ktime_t tstamp;
    char cb[48] __aligned(8);
    unsigned long _skb_refdst;
    void (*destructor)(struct sk_buff* skb);
    unsigned int len;
    unsigned int data_len;
    u16 mac_len;
    u16 hdr_len;
    u16 queue_mapping;
    u8 cloned : 1, nohdr : 1, fclone : 2, peeked : 1, head_frag : 1, pfmemalloc : 1, pp_recycle : 1;
    u8 pkt_type : 3, ignore_df : 1, dst_pending_confirm : 1, ip_summed : 2, ooo_okay : 1;
    u8 l4_hash : 1, sw_hash : 1, wifi_acked_valid : 1, wifi_acked : 1, no_fcs : 1, encapsulation : 1,
        encap_hdr_csum : 1, csum_valid : 1;
    u8 inner_protocol_type : 1, remcsum_offload : 1, offload_fwd_mark : 1, tc_skip_classify : 1,
        csum_not_inet : 1, decrypted : 1, slow_gro : 1, mono_delivery_time : 1;
    u16 tc_index;
    union {
        u32 csum;
        struct {
            u16 csum_start;
            u16 csum_offset;
        };
    };
    u32 priority;
    int skb_iif;
    u32 hash;
    union {
        u32 vlan_all;
        struct {
            __be16 vlan_proto;
            u16 vlan_tci;
        };
    };
    u32 napi_id;
    u32 mark;
    __be16 inner_protocol;
    u16 inner_transport_header;
    u16 inner_network_header;
    u16 inner_mac_header;
    __be16 protocol;
    u16 transport_header;
    u16 network_header;
    u16 mac_header;
    unsigned char* tail;
    unsigned char* end;
    unsigned char* head;
    unsigned char* data;
    unsigned int truesize;
    refcount_t users;
    /* LinuxKPI private */
    struct page* lkpi_pages[4]; /* references taken for skb_add_rx_frag() */
    struct skb_shared_info lkpi_shinfo;
};

#define SKB_ALLOC_FCLONE 0x01
#define SKB_ALLOC_RX 0x02

/* ---- allocation ---- */
struct sk_buff* alloc_skb(unsigned int size, gfp_t gfp);
#define __alloc_skb(size, gfp, flags, node) alloc_skb(size, gfp)
#define alloc_skb_fclone(size, gfp) alloc_skb(size, gfp)
struct sk_buff* netdev_alloc_skb(struct net_device* dev, unsigned int len);
#define __netdev_alloc_skb(dev, len, gfp) netdev_alloc_skb(dev, len)
struct sk_buff* napi_alloc_skb(struct napi_struct* napi, unsigned int len);
#define dev_alloc_skb(len) netdev_alloc_skb(NULL, len)
#define __dev_alloc_skb(len, gfp) netdev_alloc_skb(NULL, len)
static inline struct sk_buff* netdev_alloc_skb_ip_align(struct net_device* dev, unsigned int len) {
    return netdev_alloc_skb(dev, len);
}
#define napi_alloc_skb_ip_align napi_alloc_skb
void kfree_skb(struct sk_buff* skb);
void kfree_skb_list(struct sk_buff* segs);
#define kfree_skb_reason(skb, reason) kfree_skb(skb)
#define kfree_skb_list_reason(skb, reason) kfree_skb_list(skb)
#define sk_skb_reason_drop(sk, skb, reason) kfree_skb(skb)
#define consume_skb kfree_skb
#define __kfree_skb kfree_skb
#define dev_kfree_skb kfree_skb
#define dev_kfree_skb_any kfree_skb
#define dev_kfree_skb_irq kfree_skb
#define dev_consume_skb_any kfree_skb
#define dev_consume_skb_irq kfree_skb
#define dev_kfree_skb_any_reason(skb, r) kfree_skb(skb)
#define napi_consume_skb(skb, budget) kfree_skb(skb)
struct sk_buff* skb_copy(const struct sk_buff* skb, gfp_t gfp);
struct sk_buff* skb_copy_expand(const struct sk_buff* skb, int newheadroom, int newtailroom, gfp_t gfp);
struct sk_buff* skb_clone(struct sk_buff* skb, gfp_t gfp);
#define pskb_copy(skb, gfp) skb_copy(skb, gfp)
#define __pskb_copy(skb, h, gfp) skb_copy(skb, gfp)
int pskb_expand_head(struct sk_buff* skb, int nhead, int ntail, gfp_t gfp);
struct sk_buff* skb_realloc_headroom(struct sk_buff* skb, unsigned int headroom);
struct sk_buff* skb_clone_sk(struct sk_buff* skb);
void skb_complete_wifi_ack(struct sk_buff* skb, bool acked);
void skb_add_rx_frag(struct sk_buff* skb, int i, struct page* page, int off, int size, unsigned int truesize);
int skb_copy_bits(const struct sk_buff* skb, int offset, void* to, int len);
int skb_store_bits(struct sk_buff* skb, int offset, const void* from, int len);
struct sk_buff* skb_gso_segment(struct sk_buff* skb, u64 features);
#define __skb_gso_segment(skb, f, tx) skb_gso_segment(skb, f)
u32 skb_get_hash(struct sk_buff* skb);
#define skb_get_hash_raw(skb) ((skb)->hash)
void skb_trim(struct sk_buff* skb, unsigned int len);
int skb_checksum_help(struct sk_buff* skb);
void skb_tstamp_tx(struct sk_buff* orig, struct skb_shared_hwtstamps* hwtstamps);
void skb_complete_tx_timestamp(struct sk_buff* skb, struct skb_shared_hwtstamps* hwtstamps);
struct sk_buff* skb_dequeue(struct sk_buff_head* list);
struct sk_buff* skb_dequeue_tail(struct sk_buff_head* list);
void skb_queue_purge(struct sk_buff_head* list);
#define skb_queue_purge_reason(l, r) skb_queue_purge(l)
void skb_queue_tail(struct sk_buff_head* list, struct sk_buff* newsk);
void skb_queue_head(struct sk_buff_head* list, struct sk_buff* newsk);
void skb_unlink(struct sk_buff* skb, struct sk_buff_head* list);
void skb_append(struct sk_buff* old, struct sk_buff* newsk, struct sk_buff_head* list);

#define skb_shinfo(skb) (&(skb)->lkpi_shinfo)
static inline struct sk_buff* skb_get(struct sk_buff* skb) { refcount_inc(&skb->users); return skb; }
static inline int skb_shared(const struct sk_buff* skb) { return refcount_read(&skb->users) != 1; }
static inline int skb_cloned(const struct sk_buff* skb) { return 0; }
static inline int skb_header_cloned(const struct sk_buff* skb) { return 0; }
static inline bool skb_unref(struct sk_buff* skb) {
    if (refcount_read(&skb->users) == 1) return true;
    return refcount_dec_and_test(&skb->users);
}
static inline struct sk_buff* skb_share_check(struct sk_buff* skb, gfp_t pri) {
    if (skb_shared(skb)) {
        struct sk_buff* nskb = skb_clone(skb, pri);
        kfree_skb(skb);
        skb = nskb;
    }
    return skb;
}
static inline struct sk_buff* skb_unshare(struct sk_buff* skb, gfp_t pri) { return skb; }
static inline bool skb_clone_writable(const struct sk_buff* skb, unsigned int len) { return true; }
static inline int skb_ensure_writable(struct sk_buff* skb, unsigned int write_len) { return write_len <= skb->len ? 0 : -ENOMEM; }

/* ---- geometry ---- */
static inline bool skb_is_nonlinear(const struct sk_buff* skb) { return skb->data_len; }
static inline unsigned int skb_headlen(const struct sk_buff* skb) { return skb->len - skb->data_len; }
static inline unsigned char* skb_tail_pointer(const struct sk_buff* skb) { return skb->tail; }
static inline void skb_reset_tail_pointer(struct sk_buff* skb) { skb->tail = skb->data; }
static inline void skb_set_tail_pointer(struct sk_buff* skb, const int offset) { skb->tail = skb->data + offset; }
static inline unsigned char* skb_end_pointer(const struct sk_buff* skb) { return skb->end; }
static inline unsigned int skb_end_offset(const struct sk_buff* skb) { return (unsigned int)(skb->end - skb->head); }
static inline unsigned int skb_headroom(const struct sk_buff* skb) { return (unsigned int)(skb->data - skb->head); }
static inline int skb_tailroom(const struct sk_buff* skb) { return (int)(skb->end - skb->tail); }
static inline int skb_availroom(const struct sk_buff* skb) { return skb_tailroom(skb); }
static inline void skb_reserve(struct sk_buff* skb, int len) { skb->data += len; skb->tail += len; }
static inline void* __skb_put(struct sk_buff* skb, unsigned int len) {
    void* old = skb->tail;
    skb->tail += len;
    skb->len += len;
    return old;
}
void* skb_put(struct sk_buff* skb, unsigned int len);
static inline void* skb_put_zero(struct sk_buff* skb, unsigned int len) { void* p = skb_put(skb, len); memset(p, 0, len); return p; }
static inline void* skb_put_data(struct sk_buff* skb, const void* d, unsigned int len) { void* p = skb_put(skb, len); memcpy(p, d, len); return p; }
static inline void* __skb_put_zero(struct sk_buff* skb, unsigned int len) { void* p = __skb_put(skb, len); memset(p, 0, len); return p; }
static inline void* __skb_put_data(struct sk_buff* skb, const void* d, unsigned int len) { void* p = __skb_put(skb, len); memcpy(p, d, len); return p; }
static inline void skb_put_u8(struct sk_buff* skb, u8 val) { *(u8*)skb_put(skb, 1) = val; }
static inline void __skb_put_u8(struct sk_buff* skb, u8 val) { *(u8*)__skb_put(skb, 1) = val; }
static inline void* __skb_push(struct sk_buff* skb, unsigned int len) { skb->data -= len; skb->len += len; return skb->data; }
void* skb_push(struct sk_buff* skb, unsigned int len);
static inline void* __skb_pull(struct sk_buff* skb, unsigned int len) { skb->len -= len; return skb->data += len; }
static inline void* skb_pull(struct sk_buff* skb, unsigned int len) {
    if (len > skb->len) return NULL;
    return __skb_pull(skb, len);
}
#define pskb_pull skb_pull
#define skb_pull_inline skb_pull
static inline void* skb_pull_data(struct sk_buff* skb, size_t len) {
    void* data = skb->data;
    if (skb->len < len) return NULL;
    skb_pull(skb, len);
    return data;
}
static inline bool pskb_may_pull(struct sk_buff* skb, unsigned int len) { return len <= skb->len; }
#define pskb_network_may_pull(skb, len) pskb_may_pull(skb, skb_network_offset(skb) + (len))
static inline void __skb_trim(struct sk_buff* skb, unsigned int len) { skb->len = len; skb->tail = skb->data + len; }
static inline int __pskb_trim(struct sk_buff* skb, unsigned int len) { __skb_trim(skb, len); return 0; }
static inline int pskb_trim(struct sk_buff* skb, unsigned int len) { if (len < skb->len) __skb_trim(skb, len); return 0; }
#define pskb_trim_rcsum pskb_trim
static inline int skb_linearize(struct sk_buff* skb) { return 0; }
static inline int __skb_linearize(struct sk_buff* skb) { return 0; }
static inline bool skb_needs_linearize(struct sk_buff* skb, u64 features) { return false; }
static inline int skb_linearize_cow(struct sk_buff* skb) { return 0; }
static inline bool skb_has_frag_list(const struct sk_buff* skb) { return false; }
static inline int skb_cow(struct sk_buff* skb, unsigned int headroom) {
    int delta = headroom > skb_headroom(skb) ? (int)(headroom - skb_headroom(skb)) : 0;
    return delta ? pskb_expand_head(skb, ALIGN(delta, NET_SKB_PAD), 0, GFP_ATOMIC) : 0;
}
#define skb_cow_head skb_cow
static inline int skb_padto(struct sk_buff* skb, unsigned int len) {
    unsigned int size = skb->len;
    if (size >= len) return 0;
    if (skb_tailroom(skb) < (int)(len - size) && pskb_expand_head(skb, 0, len - size, GFP_ATOMIC)) {
        kfree_skb(skb);
        return -ENOMEM;
    }
    memset(skb->data + size, 0, len - size);
    return 0;
}
static inline int skb_put_padto(struct sk_buff* skb, unsigned int len) {
    unsigned int size = skb->len;
    if (size < len) {
        if (skb_padto(skb, len)) return -ENOMEM;
        __skb_put(skb, len - size);
    }
    return 0;
}
#define __skb_put_padto(skb, len, free) skb_put_padto(skb, len)
static inline int skb_pad(struct sk_buff* skb, int pad) { return skb_padto(skb, skb->len + pad) ? -ENOMEM : 0; }
static inline void skb_copy_to_linear_data(struct sk_buff* skb, const void* from, unsigned int len) { memcpy(skb->data, from, len); }
static inline void skb_copy_to_linear_data_offset(struct sk_buff* skb, int off, const void* from, unsigned int len) { memcpy(skb->data + off, from, len); }
static inline void skb_copy_from_linear_data(const struct sk_buff* skb, void* to, unsigned int len) { memcpy(to, skb->data, len); }
static inline void skb_copy_from_linear_data_offset(const struct sk_buff* skb, int off, void* to, unsigned int len) { memcpy(to, skb->data + off, len); }
static inline void skb_copy_and_csum_dev(const struct sk_buff* skb, u8* to) { memcpy(to, skb->data, skb->len); }
static inline void* skb_header_pointer(const struct sk_buff* skb, int offset, int len, void* buffer) {
    if (offset < 0 || offset + len > (int)skb->len) return NULL;
    return skb->data + offset;
}
static inline unsigned int skb_pagelen(const struct sk_buff* skb) { return skb_headlen(skb); }
static inline void skb_len_add(struct sk_buff* skb, int delta) { skb->len += delta; }

/* ---- headers ---- */
static inline unsigned char* skb_mac_header(const struct sk_buff* skb) { return skb->head + skb->mac_header; }
static inline int skb_mac_offset(const struct sk_buff* skb) { return (int)(skb_mac_header(skb) - skb->data); }
static inline bool skb_mac_header_was_set(const struct sk_buff* skb) { return skb->mac_header != (u16)~0U; }
static inline void skb_unset_mac_header(struct sk_buff* skb) { skb->mac_header = (u16)~0U; }
static inline void skb_reset_mac_header(struct sk_buff* skb) { skb->mac_header = (u16)(skb->data - skb->head); }
static inline void skb_set_mac_header(struct sk_buff* skb, const int offset) { skb_reset_mac_header(skb); skb->mac_header += offset; }
static inline void skb_reset_mac_len(struct sk_buff* skb) { skb->mac_len = skb->network_header - skb->mac_header; }
static inline unsigned char* skb_network_header(const struct sk_buff* skb) { return skb->head + skb->network_header; }
static inline void skb_reset_network_header(struct sk_buff* skb) { skb->network_header = (u16)(skb->data - skb->head); }
static inline void skb_set_network_header(struct sk_buff* skb, const int offset) { skb_reset_network_header(skb); skb->network_header += offset; }
static inline int skb_network_offset(const struct sk_buff* skb) { return (int)(skb_network_header(skb) - skb->data); }
static inline unsigned char* skb_transport_header(const struct sk_buff* skb) { return skb->head + skb->transport_header; }
static inline void skb_reset_transport_header(struct sk_buff* skb) { skb->transport_header = (u16)(skb->data - skb->head); }
static inline void skb_set_transport_header(struct sk_buff* skb, const int offset) { skb_reset_transport_header(skb); skb->transport_header += offset; }
static inline int skb_transport_offset(const struct sk_buff* skb) { return (int)(skb_transport_header(skb) - skb->data); }
static inline bool skb_transport_header_was_set(const struct sk_buff* skb) { return skb->transport_header != (u16)~0U; }
static inline void skb_set_inner_transport_header(struct sk_buff* skb, const int offset) { skb->inner_transport_header = (u16)(skb->data - skb->head + offset); }
static inline int skb_checksum_start_offset(const struct sk_buff* skb) { return skb->csum_start - skb_headroom(skb); }
static inline void skb_postpull_rcsum(struct sk_buff* skb, const void* start, unsigned int len) {}
static inline void skb_postpush_rcsum(struct sk_buff* skb, const void* start, unsigned int len) {}
static inline void skb_checksum_none_assert(const struct sk_buff* skb) {}
static inline int skb_csum_hwoffload_help(struct sk_buff* skb, u64 features) { return 0; }

/* ---- misc metadata ---- */
static inline u16 skb_get_queue_mapping(const struct sk_buff* skb) { return skb->queue_mapping; }
static inline void skb_set_queue_mapping(struct sk_buff* skb, u16 queue_mapping) { skb->queue_mapping = queue_mapping; }
static inline void skb_copy_queue_mapping(struct sk_buff* to, const struct sk_buff* from) { to->queue_mapping = from->queue_mapping; }
static inline void skb_record_rx_queue(struct sk_buff* skb, u16 q) { skb->queue_mapping = q + 1; }
static inline struct skb_shared_hwtstamps* skb_hwtstamps(struct sk_buff* skb) { return &skb_shinfo(skb)->hwtstamps; }
static inline void skb_tx_timestamp(struct sk_buff* skb) {}
static inline void skb_orphan(struct sk_buff* skb) {
    if (skb->destructor) { skb->destructor(skb); skb->destructor = NULL; }
    skb->sk = NULL;
}
static inline void skb_dst_drop(struct sk_buff* skb) { skb->_skb_refdst = 0; }
static inline void skb_ext_reset(struct sk_buff* skb) {}
static inline void skb_mark_not_on_list(struct sk_buff* skb) { skb->next = NULL; }
static inline void skb_list_del_init(struct sk_buff* skb) { __list_del(skb->list.prev, skb->list.next); skb_mark_not_on_list(skb); }
#define skb_list_walk_safe(first, skb, next_skb) \
    for ((skb) = (first), (next_skb) = (skb) ? (skb)->next : NULL; (skb); (skb) = (next_skb), (next_skb) = (skb) ? (skb)->next : NULL)
static inline u64 skb_get_kcov_handle(struct sk_buff* skb) { return 0; }
static inline void skb_set_kcov_handle(struct sk_buff* skb, const u64 h) {}
static inline void skb_scrub_packet(struct sk_buff* skb, bool xnet) {}
static inline void nf_reset_ct(struct sk_buff* skb) {}
static inline void skb_clear_hash(struct sk_buff* skb) { skb->hash = 0; }
static inline void skb_set_hash(struct sk_buff* skb, u32 hash, int type) { skb->hash = hash; }
static inline bool skb_is_gso(const struct sk_buff* skb) { return skb_shinfo(skb)->gso_size; }
static inline bool skb_is_gso_v6(const struct sk_buff* skb) { return false; }
static inline unsigned int skb_frag_size(const skb_frag_t* f) { return f->len; }
static inline struct page* skb_frag_page(const skb_frag_t* f) { return f->netmem; }
static inline void* skb_frag_address(const skb_frag_t* f) { return (char*)lkpi_page_address(f->netmem) + f->offset; }
static inline unsigned int skb_frag_off(const skb_frag_t* f) { return f->offset; }
#define skb_frag_dma_map(dev, frag, off, size, dir) dma_map_single(dev, (char*)skb_frag_address(frag) + (off), size, dir)
#define skb_walk_frags(skb, iter) for (iter = NULL; iter; iter = iter->next)

/* VLAN tags: not offloaded. */
static inline bool skb_vlan_tag_present(const struct sk_buff* skb) { return false; }
static inline u16 skb_vlan_tag_get(const struct sk_buff* skb) { return 0; }
static inline void __vlan_hwaccel_put_tag(struct sk_buff* skb, u16 proto, u16 tci) {}
#define swab16(x) __builtin_bswap16(x)
#define swab32(x) __builtin_bswap32(x)

/* ---- queues ---- */
struct sk_buff_head {
    union {
        struct {
            struct sk_buff* next;
            struct sk_buff* prev;
        };
        struct sk_buff_list {
            struct sk_buff* next;
            struct sk_buff* prev;
        } list;
    };
    u32 qlen;
    spinlock_t lock;
};

static inline void __skb_queue_head_init(struct sk_buff_head* q) { q->next = q->prev = (struct sk_buff*)q; q->qlen = 0; }
static inline void skb_queue_head_init(struct sk_buff_head* q) { spin_lock_init(&q->lock); __skb_queue_head_init(q); }
#define skb_queue_head_init_class(q, k) skb_queue_head_init(q)
static inline u32 skb_queue_len(const struct sk_buff_head* q) { return q->qlen; }
static inline u32 skb_queue_len_lockless(const struct sk_buff_head* q) { return READ_ONCE(q->qlen); }
static inline int skb_queue_empty(const struct sk_buff_head* q) { return q->next == (const struct sk_buff*)q; }
static inline bool skb_queue_empty_lockless(const struct sk_buff_head* q) { return READ_ONCE(q->next) == (const struct sk_buff*)q; }
static inline bool skb_queue_is_last(const struct sk_buff_head* q, const struct sk_buff* skb) { return skb->next == (const struct sk_buff*)q; }
static inline bool skb_queue_is_first(const struct sk_buff_head* q, const struct sk_buff* skb) { return skb->prev == (const struct sk_buff*)q; }
static inline struct sk_buff* skb_queue_next(const struct sk_buff_head* q, const struct sk_buff* skb) { return skb->next; }
static inline struct sk_buff* skb_queue_prev(const struct sk_buff_head* q, const struct sk_buff* skb) { return skb->prev; }
static inline struct sk_buff* skb_peek(const struct sk_buff_head* q) {
    struct sk_buff* skb = q->next;
    return skb == (struct sk_buff*)q ? NULL : skb;
}
#define __skb_peek(q) ((q)->next)
static inline struct sk_buff* skb_peek_next(struct sk_buff* skb, const struct sk_buff_head* q) {
    struct sk_buff* next = skb->next;
    return next == (struct sk_buff*)q ? NULL : next;
}
static inline struct sk_buff* skb_peek_tail(const struct sk_buff_head* q) {
    struct sk_buff* skb = READ_ONCE(q->prev);
    return skb == (struct sk_buff*)q ? NULL : skb;
}
static inline void __skb_insert(struct sk_buff* newsk, struct sk_buff* prev, struct sk_buff* next, struct sk_buff_head* list) {
    newsk->next = next;
    newsk->prev = prev;
    next->prev = newsk;
    prev->next = newsk;
    list->qlen++;
}
static inline void __skb_queue_after(struct sk_buff_head* list, struct sk_buff* prev, struct sk_buff* newsk) { __skb_insert(newsk, prev, prev->next, list); }
static inline void __skb_queue_before(struct sk_buff_head* list, struct sk_buff* next, struct sk_buff* newsk) { __skb_insert(newsk, next->prev, next, list); }
static inline void __skb_queue_head(struct sk_buff_head* list, struct sk_buff* newsk) { __skb_queue_after(list, (struct sk_buff*)list, newsk); }
static inline void __skb_queue_tail(struct sk_buff_head* list, struct sk_buff* newsk) { __skb_queue_before(list, (struct sk_buff*)list, newsk); }
static inline void __skb_unlink(struct sk_buff* skb, struct sk_buff_head* list) {
    struct sk_buff *next, *prev;
    list->qlen--;
    next = skb->next;
    prev = skb->prev;
    skb->next = skb->prev = NULL;
    next->prev = prev;
    prev->next = next;
}
static inline struct sk_buff* __skb_dequeue(struct sk_buff_head* list) {
    struct sk_buff* skb = skb_peek(list);
    if (skb) __skb_unlink(skb, list);
    return skb;
}
static inline struct sk_buff* __skb_dequeue_tail(struct sk_buff_head* list) {
    struct sk_buff* skb = skb_peek_tail(list);
    if (skb) __skb_unlink(skb, list);
    return skb;
}
static inline void __skb_queue_purge(struct sk_buff_head* list) {
    struct sk_buff* skb;
    while ((skb = __skb_dequeue(list)) != NULL) kfree_skb(skb);
}
#define __skb_queue_purge_reason(l, r) __skb_queue_purge(l)
static inline void __skb_queue_splice(const struct sk_buff_head* list, struct sk_buff* prev, struct sk_buff* next) {
    struct sk_buff* first = list->next;
    struct sk_buff* last = list->prev;
    first->prev = prev;
    prev->next = first;
    last->next = next;
    next->prev = last;
}
static inline void skb_queue_splice(const struct sk_buff_head* list, struct sk_buff_head* head) {
    if (!skb_queue_empty(list)) { __skb_queue_splice(list, (struct sk_buff*)head, head->next); head->qlen += list->qlen; }
}
static inline void skb_queue_splice_init(struct sk_buff_head* list, struct sk_buff_head* head) {
    if (!skb_queue_empty(list)) { __skb_queue_splice(list, (struct sk_buff*)head, head->next); head->qlen += list->qlen; __skb_queue_head_init(list); }
}
static inline void skb_queue_splice_tail(const struct sk_buff_head* list, struct sk_buff_head* head) {
    if (!skb_queue_empty(list)) { __skb_queue_splice(list, head->prev, (struct sk_buff*)head); head->qlen += list->qlen; }
}
static inline void skb_queue_splice_tail_init(struct sk_buff_head* list, struct sk_buff_head* head) {
    if (!skb_queue_empty(list)) { __skb_queue_splice(list, head->prev, (struct sk_buff*)head); head->qlen += list->qlen; __skb_queue_head_init(list); }
}
#define skb_queue_walk(queue, skb) for (skb = (queue)->next; skb != (struct sk_buff*)(queue); skb = skb->next)
#define skb_queue_walk_safe(queue, skb, tmp) \
    for (skb = (queue)->next, tmp = skb->next; skb != (struct sk_buff*)(queue); skb = tmp, tmp = skb->next)
#define skb_queue_walk_from(queue, skb) for (; skb != (struct sk_buff*)(queue); skb = skb->next)
#define skb_queue_walk_from_safe(queue, skb, tmp) for (tmp = skb->next; skb != (struct sk_buff*)(queue); skb = tmp, tmp = skb->next)
#define skb_queue_reverse_walk(queue, skb) for (skb = (queue)->prev; skb != (struct sk_buff*)(queue); skb = skb->prev)
#define skb_queue_reverse_walk_safe(queue, skb, tmp) \
    for (skb = (queue)->prev, tmp = skb->prev; skb != (struct sk_buff*)(queue); skb = tmp, tmp = skb->prev)

#include <linux/if_ether.h>
static inline struct ethhdr* eth_hdr(const struct sk_buff* skb) { return (struct ethhdr*)skb_mac_header(skb); }
static inline struct ethhdr* skb_eth_hdr(const struct sk_buff* skb) { return (struct ethhdr*)skb->data; }

/* ---- pages (for drivers that receive into pages) ---- */


LKPI_EXTERN_C_END

#endif
