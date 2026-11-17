#ifndef _LKPI_LINUX_SKBUFF_H
#define _LKPI_LINUX_SKBUFF_H

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

LKPI_EXTERN_C_BEGIN

struct net_device;
struct napi_struct;

/* A packet buffer: linear only (no fragments). Layout: head .. data .. tail .. end. */
struct sk_buff {
    struct sk_buff *next, *prev;
    struct net_device* dev;
    unsigned int len;      /* bytes between data and tail */
    unsigned int data_len; /* paged data: always 0 here */
    u16 protocol;          /* network byte order, set by eth_type_trans */
    u8 ip_summed;
    u8 pkt_type;
    u16 queue_mapping;
    u32 priority;
    unsigned char* head;
    unsigned char* data;
    unsigned char* tail;
    unsigned char* end;
    unsigned char* mac_header_ptr;
    char cb[48];
};

#define CHECKSUM_NONE 0
#define CHECKSUM_UNNECESSARY 1
#define CHECKSUM_COMPLETE 2
#define CHECKSUM_PARTIAL 3
#define NET_SKB_PAD 32
#define NET_IP_ALIGN 2

struct sk_buff* alloc_skb(unsigned int size, gfp_t gfp);
struct sk_buff* netdev_alloc_skb(struct net_device* dev, unsigned int len);
struct sk_buff* napi_alloc_skb(struct napi_struct* napi, unsigned int len);
#define dev_alloc_skb(len) netdev_alloc_skb(NULL, len)
#define netdev_alloc_skb_ip_align(dev, len) netdev_alloc_skb(dev, len)
void kfree_skb(struct sk_buff* skb);
#define consume_skb kfree_skb
#define dev_kfree_skb kfree_skb
#define dev_kfree_skb_any kfree_skb
#define dev_kfree_skb_irq kfree_skb
#define dev_consume_skb_any kfree_skb
#define napi_consume_skb(skb, budget) kfree_skb(skb)
struct sk_buff* skb_copy(const struct sk_buff* skb, gfp_t gfp);

static inline unsigned char* skb_put(struct sk_buff* skb, unsigned int len) {
    unsigned char* old = skb->tail;
    skb->tail += len;
    skb->len += len;
    return old;
}
static inline void* skb_put_zero(struct sk_buff* skb, unsigned int len) { void* p = skb_put(skb, len); memset(p, 0, len); return p; }
static inline void* skb_put_data(struct sk_buff* skb, const void* d, unsigned int len) { void* p = skb_put(skb, len); memcpy(p, d, len); return p; }
static inline unsigned char* skb_push(struct sk_buff* skb, unsigned int len) { skb->data -= len; skb->len += len; return skb->data; }
static inline unsigned char* skb_pull(struct sk_buff* skb, unsigned int len) {
    if (len > skb->len) return NULL;
    skb->data += len; skb->len -= len; return skb->data;
}
static inline void skb_reserve(struct sk_buff* skb, int len) { skb->data += len; skb->tail += len; }
static inline unsigned int skb_headroom(const struct sk_buff* skb) { return (unsigned int)(skb->data - skb->head); }
static inline int skb_tailroom(const struct sk_buff* skb) { return (int)(skb->end - skb->tail); }
static inline unsigned int skb_headlen(const struct sk_buff* skb) { return skb->len; }
static inline void skb_trim(struct sk_buff* skb, unsigned int len) { if (skb->len > len) { skb->len = len; skb->tail = skb->data + len; } }
static inline int skb_is_nonlinear(const struct sk_buff* skb) { return 0; }
static inline int skb_linearize(struct sk_buff* skb) { return 0; }
static inline int skb_padto(struct sk_buff* skb, unsigned int len) {
    if (skb->len >= len) return 0;
    if (skb_tailroom(skb) < (int)(len - skb->len)) { kfree_skb(skb); return -ENOMEM; }
    memset(skb->tail, 0, len - skb->len);
    return 0;
}
static inline int skb_put_padto(struct sk_buff* skb, unsigned int len) {
    int r = skb_padto(skb, len);
    if (!r && skb->len < len) skb_put(skb, len - skb->len);
    return r;
}
static inline void skb_copy_to_linear_data(struct sk_buff* skb, const void* from, unsigned int len) { memcpy(skb->data, from, len); }
static inline void skb_copy_to_linear_data_offset(struct sk_buff* skb, int off, const void* from, unsigned int len) { memcpy(skb->data + off, from, len); }
static inline void skb_copy_from_linear_data(const struct sk_buff* skb, void* to, unsigned int len) { memcpy(to, skb->data, len); }
static inline int skb_copy_bits(const struct sk_buff* skb, int off, void* to, int len) { memcpy(to, skb->data + off, len); return 0; }
/* Copy the packet (checksumming is done in software by our stack, so plain copy). */
static inline void skb_copy_and_csum_dev(const struct sk_buff* skb, u8* to) { memcpy(to, skb->data, skb->len); }
static inline unsigned char* skb_mac_header(const struct sk_buff* skb) { return skb->mac_header_ptr; }
static inline void skb_reset_mac_header(struct sk_buff* skb) { skb->mac_header_ptr = skb->data; }
static inline void skb_checksum_none_assert(const struct sk_buff* skb) {}
static inline void skb_tx_timestamp(struct sk_buff* skb) {}
static inline void skb_record_rx_queue(struct sk_buff* skb, u16 q) {}
#define skb_get_queue_mapping(skb) ((skb)->queue_mapping)

/* Paged fragments: Syluxium's buffers are always linear, so every skb has
 * zero fragments and no segmentation offload. */
#define MAX_SKB_FRAGS 17
typedef struct skb_frag {
    void* page_virt;
    unsigned int offset;
    unsigned int size;
} skb_frag_t;
struct skb_shared_info {
    u8 nr_frags;
    u8 tx_flags;
    unsigned short gso_size, gso_segs;
    unsigned int gso_type;
    skb_frag_t frags[MAX_SKB_FRAGS];
};
extern struct skb_shared_info lkpi_empty_shinfo;
#define skb_shinfo(skb) ((void)(skb), &lkpi_empty_shinfo)
static inline unsigned int skb_frag_size(const skb_frag_t* f) { return f->size; }
static inline void* skb_frag_address(const skb_frag_t* f) { return (char*)f->page_virt + f->offset; }
#define skb_frag_dma_map(dev, frag, off, size, dir) dma_map_single(dev, (char*)skb_frag_address(frag) + (off), size, dir)
static inline int skb_is_gso(const struct sk_buff* skb) { return 0; }
static inline int skb_transport_offset(const struct sk_buff* skb) { return 34; }
static inline int skb_checksum_help(struct sk_buff* skb) { return 0; }
#define dev_consume_skb_irq kfree_skb

/* VLAN tags: not offloaded (the stack doesn't use VLANs yet). */
static inline bool skb_vlan_tag_present(const struct sk_buff* skb) { return false; }
static inline u16 skb_vlan_tag_get(const struct sk_buff* skb) { return 0; }
static inline void __vlan_hwaccel_put_tag(struct sk_buff* skb, u16 proto, u16 tci) {}
#define swab16(x) __builtin_bswap16(x)
#define swab32(x) __builtin_bswap32(x)

/* Queues of buffers. */
struct sk_buff_head {
    struct sk_buff *next, *prev;
    u32 qlen;
    spinlock_t lock;
};
static inline void skb_queue_head_init(struct sk_buff_head* q) { q->next = q->prev = (struct sk_buff*)q; q->qlen = 0; spin_lock_init(&q->lock); }
static inline u32 skb_queue_len(const struct sk_buff_head* q) { return q->qlen; }
static inline int skb_queue_empty(const struct sk_buff_head* q) { return q->next == (const struct sk_buff*)q; }
static inline void __skb_queue_tail(struct sk_buff_head* q, struct sk_buff* skb) {
    struct sk_buff* last = q->prev; skb->next = (struct sk_buff*)q; skb->prev = last; last->next = skb; q->prev = skb; q->qlen++;
}
static inline struct sk_buff* __skb_dequeue(struct sk_buff_head* q) {
    struct sk_buff* skb = q->next;
    if (skb == (struct sk_buff*)q) return NULL;
    q->next = skb->next; skb->next->prev = (struct sk_buff*)q; skb->next = skb->prev = NULL; q->qlen--; return skb;
}
static inline void skb_queue_tail(struct sk_buff_head* q, struct sk_buff* skb) { unsigned long f; spin_lock_irqsave(&q->lock, f); __skb_queue_tail(q, skb); spin_unlock_irqrestore(&q->lock, f); }
static inline struct sk_buff* skb_dequeue(struct sk_buff_head* q) { unsigned long f; struct sk_buff* s; spin_lock_irqsave(&q->lock, f); s = __skb_dequeue(q); spin_unlock_irqrestore(&q->lock, f); return s; }
static inline void skb_queue_purge(struct sk_buff_head* q) { struct sk_buff* s; while ((s = skb_dequeue(q))) kfree_skb(s); }

LKPI_EXTERN_C_END

#endif
