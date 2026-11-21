// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI: socket buffers (linear only) and pages.
 *
 * Every sk_buff owns one kmalloc'ed buffer (head .. end). Operations that
 * would share or chain buffers in Linux copy instead: skb_clone() is a full
 * copy and skb_add_rx_frag() appends the fragment's bytes to the linear
 * part (growing the buffer when needed), keeping a page reference until the
 * skb is freed, as the caller expects.
 */
#include <linux/lkpi_all.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <net/sock.h>
#include <net/dsfield.h>
#include <net/page_pool/types.h>

/* ---- build_skb: an skb around a buffer the driver filled ----
 * The buffer's bytes are copied into an skb of our own (skbs here own one
 * kmalloc'ed buffer) and the buffer is released: a page fragment
 * (frag_size != 0: its page, e.g. from a page pool) or kmalloc'ed. */

struct sk_buff* build_skb(void* data, unsigned int frag_size)
{
	unsigned int size = frag_size ? SKB_WITH_OVERHEAD(frag_size) : (unsigned int)ksize(data);
	struct sk_buff* skb = alloc_skb(size, GFP_ATOMIC);
	if (skb)
		memcpy(skb->head, data, size);
	if (frag_size) {
		struct page* page = virt_to_head_page(data);
		if (page->pp)
			page_pool_put_full_page(page->pp, page, false);
		else
			put_page(page);
	} else {
		kfree(data);
	}
	return skb;
}

struct sk_buff* napi_build_skb(void* data, unsigned int frag_size)
{
	return build_skb(data, frag_size);
}

/* ---- page pools (net/page_pool/types.h): a page per buffer, no recycling ---- */

struct page_pool* page_pool_create(const struct page_pool_params* params)
{
	struct page_pool* pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return ERR_PTR(-ENOMEM);
	pool->p = *params;
	return pool;
}

void page_pool_destroy(struct page_pool* pool)
{
	kfree(pool);
}

struct page* page_pool_alloc_frag(struct page_pool* pool, unsigned int* offset, unsigned int size, gfp_t gfp)
{
	struct page* page = alloc_pages(gfp, pool->p.order);
	if (!page)
		return NULL;
	page->pp = pool;
	if (pool->p.flags & PP_FLAG_DMA_MAP) /* bounced under the device's mask if need be */
		page->dma_addr = lkpi_dma_map_virt(pool->p.dev, page_address(page), PAGE_SIZE << pool->p.order, pool->p.dma_dir);
	*offset = 0;
	return page;
}

void page_pool_put_full_page(struct page_pool* pool, struct page* page, bool allow_direct)
{
	if (pool && (pool->p.flags & PP_FLAG_DMA_MAP))
		dma_unmap_page(pool->p.dev, page->dma_addr, PAGE_SIZE << pool->p.order, pool->p.dma_dir);
	page->pp = NULL;
	put_page(page);
}

struct sk_buff* alloc_skb(unsigned int size, gfp_t gfp)
{
	struct sk_buff* skb = kzalloc(sizeof(*skb), gfp);
	if (!skb)
		return NULL;
	size = SKB_DATA_ALIGN(size);
	skb->head = kmalloc(size, gfp);
	if (!skb->head) {
		kfree(skb);
		return NULL;
	}
	skb->data = skb->tail = skb->head;
	skb->end = skb->head + size;
	skb->truesize = SKB_TRUESIZE(size);
	refcount_set(&skb->users, 1);
	skb->mac_header = (u16)~0U;
	skb->transport_header = (u16)~0U;
	return skb;
}

struct sk_buff* netdev_alloc_skb(struct net_device* dev, unsigned int len)
{
	struct sk_buff* skb = alloc_skb(len + NET_SKB_PAD, GFP_ATOMIC);
	if (skb) {
		skb_reserve(skb, NET_SKB_PAD);
		skb->dev = dev;
	}
	return skb;
}

struct sk_buff* napi_alloc_skb(struct napi_struct* napi, unsigned int len)
{
	return netdev_alloc_skb(napi ? napi->dev : NULL, len);
}

static void skb_release(struct sk_buff* skb)
{
	unsigned int i;
	if (skb->destructor)
		skb->destructor(skb);
	for (i = 0; i < ARRAY_SIZE(skb->lkpi_pages); i++)
		if (skb->lkpi_pages[i])
			put_page(skb->lkpi_pages[i]);
	kfree(skb->head);
	kfree(skb);
}

void kfree_skb(struct sk_buff* skb)
{
	if (!skb || !skb_unref(skb))
		return;
	skb_release(skb);
}

void kfree_skb_list(struct sk_buff* segs)
{
	while (segs) {
		struct sk_buff* next = segs->next;
		kfree_skb(segs);
		segs = next;
	}
}

void* skb_put(struct sk_buff* skb, unsigned int len)
{
	void* tmp = skb->tail;
	if (unlikely(skb->tail + len > skb->end)) {
		printk(KERN_ERR "linuxkpi: skb_put over the end (len %u, tailroom %d)\n", len, skb_tailroom(skb));
		BUG();
	}
	skb->tail += len;
	skb->len += len;
	return tmp;
}

void* skb_push(struct sk_buff* skb, unsigned int len)
{
	if (unlikely(skb->data - len < skb->head)) {
		printk(KERN_ERR "linuxkpi: skb_push under the head (len %u, headroom %u)\n", len, skb_headroom(skb));
		BUG();
	}
	skb->data -= len;
	skb->len += len;
	return skb->data;
}

void skb_trim(struct sk_buff* skb, unsigned int len)
{
	if (skb->len > len)
		__skb_trim(skb, len);
}

/* Copy everything but the buffer pointers. */
static void copy_header(struct sk_buff* n, const struct sk_buff* old)
{
	n->dev = old->dev;
	n->sk = old->sk;
	n->tstamp = old->tstamp;
	memcpy(n->cb, old->cb, sizeof(old->cb));
	n->queue_mapping = old->queue_mapping;
	n->pkt_type = old->pkt_type;
	n->ip_summed = old->ip_summed;
	n->priority = old->priority;
	n->protocol = old->protocol;
	n->mark = old->mark;
	n->hash = old->hash;
	n->vlan_all = old->vlan_all;
	n->skb_iif = old->skb_iif;
	n->wifi_acked_valid = old->wifi_acked_valid;
	n->wifi_acked = old->wifi_acked;
	n->encapsulation = old->encapsulation;
	n->csum = old->csum;
	n->mac_len = old->mac_len;
	n->lkpi_shinfo.tx_flags = old->lkpi_shinfo.tx_flags;
	n->lkpi_shinfo.hwtstamps = old->lkpi_shinfo.hwtstamps;
	n->lkpi_shinfo.tskey = old->lkpi_shinfo.tskey;
}

static void copy_offsets(struct sk_buff* n, const struct sk_buff* old, long delta)
{
	n->mac_header = old->mac_header == (u16)~0U ? (u16)~0U : (u16)(old->mac_header + delta);
	n->network_header = (u16)(old->network_header + delta);
	n->transport_header = old->transport_header == (u16)~0U ? (u16)~0U : (u16)(old->transport_header + delta);
	n->inner_transport_header = (u16)(old->inner_transport_header + delta);
}

struct sk_buff* skb_copy_expand(const struct sk_buff* skb, int newheadroom, int newtailroom, gfp_t gfp)
{
	struct sk_buff* n = alloc_skb(newheadroom + skb->len + newtailroom, gfp);
	if (!n)
		return NULL;
	skb_reserve(n, newheadroom);
	skb_put_data(n, skb->data, skb->len);
	copy_header(n, skb);
	copy_offsets(n, skb, (long)(n->data - n->head) - (long)(skb->data - skb->head));
	return n;
}

struct sk_buff* skb_copy(const struct sk_buff* skb, gfp_t gfp)
{
	return skb_copy_expand(skb, skb_headroom(skb), skb_tailroom(skb) > 0 ? skb_tailroom(skb) : 0, gfp);
}

struct sk_buff* skb_clone(struct sk_buff* skb, gfp_t gfp)
{
	struct sk_buff* n = skb_copy(skb, gfp);
	if (n)
		n->destructor = NULL;
	return n;
}

struct sk_buff* skb_realloc_headroom(struct sk_buff* skb, unsigned int headroom)
{
	int delta = headroom - skb_headroom(skb);
	return skb_copy_expand(skb, delta > 0 ? headroom : skb_headroom(skb), skb_tailroom(skb), GFP_ATOMIC);
}

int pskb_expand_head(struct sk_buff* skb, int nhead, int ntail, gfp_t gfp)
{
	unsigned int size = (unsigned int)(skb->end - skb->head) + nhead + ntail;
	unsigned char* head;
	long off;

	size = SKB_DATA_ALIGN(size);
	head = kmalloc(size, gfp);
	if (!head)
		return -ENOMEM;
	/* Copy from head to tail: headers before data stay valid. */
	memcpy(head + nhead, skb->head, skb->tail - skb->head);
	off = (head + nhead) - skb->head;
	kfree(skb->head);
	skb->head = head;
	skb->data += off;
	skb->tail += off;
	skb->end = head + size;
	skb->truesize = SKB_TRUESIZE(size);
	if (skb->mac_header != (u16)~0U)
		skb->mac_header += nhead;
	skb->network_header += nhead;
	if (skb->transport_header != (u16)~0U)
		skb->transport_header += nhead;
	skb->inner_transport_header += nhead;
	return 0;
}

void skb_add_rx_frag(struct sk_buff* skb, int i, struct page* page, int off, int size, unsigned int truesize)
{
	unsigned int k;
	if (skb_tailroom(skb) < size && pskb_expand_head(skb, 0, size - skb_tailroom(skb) + 64, GFP_ATOMIC)) {
		printk(KERN_ERR "linuxkpi: skb_add_rx_frag: out of memory\n");
		return;
	}
	skb_put_data(skb, (u8*)page_address(page) + off, size);
	/* The caller hands over one page reference (it takes one with
	 * get_page() for the skb): drop it when the skb goes. */
	for (k = 0; k < ARRAY_SIZE(skb->lkpi_pages); k++) {
		if (!skb->lkpi_pages[k]) {
			skb->lkpi_pages[k] = page;
			return;
		}
	}
	put_page(page); /* no slot: the data is copied anyway */
}

int skb_copy_bits(const struct sk_buff* skb, int offset, void* to, int len)
{
	if (offset < 0 || len < 0 || offset + len > (int)skb->len)
		return -EFAULT;
	memcpy(to, skb->data + offset, len);
	return 0;
}

int skb_store_bits(struct sk_buff* skb, int offset, const void* from, int len)
{
	if (offset < 0 || len < 0 || offset + len > (int)skb->len)
		return -EFAULT;
	memcpy(skb->data + offset, from, len);
	return 0;
}

struct sk_buff* skb_gso_segment(struct sk_buff* skb, u64 features)
{
	return ERR_PTR(-EPROTONOSUPPORT); /* no GSO packets are ever built */
}

u32 skb_get_hash(struct sk_buff* skb)
{
	if (!skb->hash) {
		u32 h = 2166136261u;
		unsigned int n = min(skb->len, 64u);
		for (unsigned int i = 0; i < n; i++)
			h = (h ^ skb->data[i]) * 16777619u;
		skb->hash = h ? h : 1;
	}
	return skb->hash;
}

int skb_checksum_help(struct sk_buff* skb)
{
	return 0; /* Syluxium's stack computes checksums in software */
}

struct sk_buff* skb_clone_sk(struct sk_buff* skb)
{
	return NULL; /* no socket error queues: no TX status reports to sockets */
}

void skb_complete_wifi_ack(struct sk_buff* skb, bool acked)
{
	kfree_skb(skb);
}

void skb_tstamp_tx(struct sk_buff* orig, struct skb_shared_hwtstamps* hwtstamps) {}
void skb_complete_tx_timestamp(struct sk_buff* skb, struct skb_shared_hwtstamps* hwtstamps) { kfree_skb(skb); }

u8 skb_get_dsfield(struct sk_buff* skb)
{
	if (skb->protocol == htons(ETH_P_IP) && skb_network_offset(skb) + 20 <= (int)skb->len)
		return ipv4_get_dsfield(ip_hdr(skb));
	if (skb->protocol == htons(ETH_P_IPV6) && skb_network_offset(skb) + 40 <= (int)skb->len)
		return ipv6_get_dsfield(ipv6_hdr(skb));
	return 0;
}

int INET_ECN_set_ce(struct sk_buff* skb)
{
	return 0;
}

/* ---- locked queue operations ---- */

struct sk_buff* skb_dequeue(struct sk_buff_head* list)
{
	unsigned long flags;
	struct sk_buff* skb;
	spin_lock_irqsave(&list->lock, flags);
	skb = __skb_dequeue(list);
	spin_unlock_irqrestore(&list->lock, flags);
	return skb;
}

struct sk_buff* skb_dequeue_tail(struct sk_buff_head* list)
{
	unsigned long flags;
	struct sk_buff* skb;
	spin_lock_irqsave(&list->lock, flags);
	skb = __skb_dequeue_tail(list);
	spin_unlock_irqrestore(&list->lock, flags);
	return skb;
}

void skb_queue_purge(struct sk_buff_head* list)
{
	struct sk_buff* skb;
	while ((skb = skb_dequeue(list)) != NULL)
		kfree_skb(skb);
}

void skb_queue_tail(struct sk_buff_head* list, struct sk_buff* newsk)
{
	unsigned long flags;
	spin_lock_irqsave(&list->lock, flags);
	__skb_queue_tail(list, newsk);
	spin_unlock_irqrestore(&list->lock, flags);
}

void skb_queue_head(struct sk_buff_head* list, struct sk_buff* newsk)
{
	unsigned long flags;
	spin_lock_irqsave(&list->lock, flags);
	__skb_queue_head(list, newsk);
	spin_unlock_irqrestore(&list->lock, flags);
}

void skb_unlink(struct sk_buff* skb, struct sk_buff_head* list)
{
	unsigned long flags;
	spin_lock_irqsave(&list->lock, flags);
	__skb_unlink(skb, list);
	spin_unlock_irqrestore(&list->lock, flags);
}

void skb_append(struct sk_buff* old, struct sk_buff* newsk, struct sk_buff_head* list)
{
	unsigned long flags;
	spin_lock_irqsave(&list->lock, flags);
	__skb_queue_after(list, old, newsk);
	spin_unlock_irqrestore(&list->lock, flags);
}

/* Pages: glue/mm.c */
