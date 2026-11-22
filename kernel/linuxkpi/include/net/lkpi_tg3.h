/* Broadcom Tigon3 (tg3): what it needs beyond the r8169/e1000e set.
 * Included (after lkpi_phy.h) for drivers/net/ethernet/broadcom. */
#pragma once
#include <net/lkpi_phy.h>

#ifndef NETIF_F_LOOPBACK
#define NETIF_F_LOOPBACK BIT_ULL(31)
#endif
#ifndef pci_channel_io_normal
#define pci_channel_io_normal 1
#endif

static inline int netif_get_num_default_rss_queues(void) { return 1; }

/* TCP options length (linux/tcp.h). */
static inline unsigned int tcp_optlen(const struct sk_buff *skb)
{
	return (tcp_hdr(skb)->doff - 5) * 4;
}

static inline char *strncat(char *dst, const char *src, size_t n)
{
	char *d = dst + strlen(dst);
	while (n-- && *src)
		*d++ = *src++;
	*d = 0;
	return dst;
}

/* Receive buffers: a page fragment (a whole page here; build_skb copies it
 * into an skb and releases the page) or a kmalloc'ed buffer. */
static inline void *napi_alloc_frag(unsigned int size)
{
	if (size > PAGE_SIZE)
		return NULL;
	struct page *p = alloc_pages(GFP_ATOMIC, 0);
	return p ? page_address(p) : NULL;
}
#define netdev_alloc_frag napi_alloc_frag
static inline void skb_free_frag(void *addr) { put_page(virt_to_head_page(addr)); }
static inline struct sk_buff *slab_build_skb(void *data) { return build_skb(data, 0); }

/* PCI Vital Product Data: not read (tg3 then names the board from the chip). */
#define PCI_VPD_RO_KEYWORD_PARTNO "PN"
#define PCI_VPD_RO_KEYWORD_MFR_ID "MN"
#define PCI_VPD_RO_KEYWORD_VENDOR0 "V0"
static inline void *pci_vpd_alloc(struct pci_dev *dev, unsigned int *size) { (void)dev; (void)size; return ERR_PTR(-ENODEV); }
static inline int pci_vpd_check_csum(const void *buf, unsigned int len) { (void)buf; (void)len; return -ENODEV; }
static inline int pci_vpd_find_ro_info_keyword(const void *buf, unsigned int len, const char *kw, unsigned int *size)
{ (void)buf; (void)len; (void)kw; (void)size; return -ENOENT; }

/* No system sleep support for tg3 here (its suspend/resume aren't built). */
#undef SIMPLE_DEV_PM_OPS
#define SIMPLE_DEV_PM_OPS(name, s, r) const struct dev_pm_ops name __attribute__((unused)) = { }
