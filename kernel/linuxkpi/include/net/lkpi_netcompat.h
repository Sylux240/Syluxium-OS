/* LinuxKPI: kernel-header pieces several Linux network drivers expect
 * (iwlwifi, r8169/phylib, e1000e) beyond the common LinuxKPI set. */
#pragma once

#include <linux/lkpi_all.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/skbuff.h>

#ifndef SMP_CACHE_BYTES
#define SMP_CACHE_BYTES 64
#endif
#ifndef ____cacheline_aligned_in_smp
#define ____cacheline_aligned_in_smp __attribute__((__aligned__(SMP_CACHE_BYTES)))
#endif

/* MSI-X (one interrupt per queue) isn't offered: drivers fall back to MSI. */
struct msix_entry {
    u32 vector;
    u16 entry;
};
static inline int pci_enable_msix_range(struct pci_dev *dev, struct msix_entry *e, int minvec, int maxvec)
{
    return -ENOSYS;
}
static inline int pci_enable_msix_exact(struct pci_dev *dev, struct msix_entry *e, int nvec) { return -ENOSYS; }

#define ns_to_timespec64(ns) \
    ({ s64 __ns = (ns); struct timespec64 __t = { __ns / 1000000000LL, __ns % 1000000000LL }; __t; })

#define IRQF_PROBE_SHARED 0

/* Receive hashing: not used by the Syluxium stack. */
#ifndef NETIF_F_RXHASH
#define NETIF_F_RXHASH BIT_ULL(42)
#endif
enum pkt_hash_types { PKT_HASH_TYPE_NONE, PKT_HASH_TYPE_L2, PKT_HASH_TYPE_L3, PKT_HASH_TYPE_L4 };

#define __netdev_alloc_skb_ip_align(dev, len, gfp) netdev_alloc_skb_ip_align(dev, len)

/* Page fragments in received packets (jumbo frames / packet split only:
 * drivers use linear buffers at the standard MTU). */
static inline void skb_fill_page_desc(struct sk_buff *skb, int i, struct page *page, int off, int size)
{
    WARN_ON_ONCE(1);
}

#define netif_msg_tx_done(p) ((p)->msg_enable & NETIF_MSG_TX_DONE)
#define netif_msg_rx_status(p) ((p)->msg_enable & NETIF_MSG_RX_STATUS)
#define device_can_wakeup(dev) false

static inline int pci_pcie_cap(struct pci_dev *dev)
{
    return pci_find_capability(dev, PCI_CAP_ID_EXP);
}

static inline void napi_synchronize(struct napi_struct *n) { synchronize_rcu(); }

/* PCIe link power states (ASPM): left as the firmware set them. */
#define PCIE_LINK_STATE_L0S (BIT(0) | BIT(1))
#define PCIE_LINK_STATE_L1 BIT(2)
static inline int pci_disable_link_state_locked(struct pci_dev *pdev, int state) { return 0; }
static inline int pci_prepare_to_sleep(struct pci_dev *dev) { return 0; }


#include <linux/tcp.h>
#define skb_tcp_all_headers(skb) (skb_transport_offset(skb) + tcp_hdrlen(skb))
#include <linux/etherdevice.h>

#define netdev_rss_key_fill(buf, len) get_random_bytes(buf, len)
#ifndef NETIF_F_HW_VLAN_CTAG_FILTER
#define NETIF_F_HW_VLAN_CTAG_FILTER BIT_ULL(23)
#endif
#define cpu_to_be16s(p) (*(p) = (__force u16)cpu_to_be16(*(p)))
#define be16_to_cpus(p) (*(p) = be16_to_cpu((__force __be16)*(p)))
#define pci_request_selected_regions_exclusive(d, mask, n) pci_request_selected_regions(d, mask, n)
static inline unsigned long dev_trans_start(struct net_device *dev) { return jiffies; }
#ifndef __MODULE_STRING
#define __MODULE_STRING(x) __stringify(x)
#endif
/* Linear packets: nothing to pull into the head. */
static inline void *__pskb_pull_tail(struct sk_buff *skb, int delta) { return skb_tail_pointer(skb); }

/* Unmap bookkeeping (no IOMMU: kept anyway, drivers read it back). */
#ifndef DEFINE_DMA_UNMAP_ADDR
#define DEFINE_DMA_UNMAP_ADDR(name) dma_addr_t name
#define DEFINE_DMA_UNMAP_LEN(name) __u32 name
#define dma_unmap_addr(ptr, name) ((ptr)->name)
#define dma_unmap_addr_set(ptr, name, val) (((ptr)->name) = (val))
#define dma_unmap_len(ptr, name) ((ptr)->name)
#define dma_unmap_len_set(ptr, name, val) (((ptr)->name) = (val))
#endif
#ifndef ____cacheline_internodealigned_in_smp
#define ____cacheline_internodealigned_in_smp ____cacheline_aligned_in_smp
#endif
#ifndef NETIF_F_NTUPLE
#define NETIF_F_NTUPLE BIT_ULL(27)
#endif
static inline s64 div_s64_rem(s64 dividend, s32 divisor, s32 *remainder)
{
    *remainder = (s32)(dividend % divisor);
    return dividend / divisor;
}
static inline void __page_frag_cache_drain(struct page *page, unsigned int count)
{
    while (count--)
        put_page(page);
}
#include <net/checksum.h>
#define SKBTX_HW_TSTAMP_USE_CYCLES (1 << 3)
#define SKBTX_HW_TSTAMP_NETDEV (1 << 5)
static inline void skb_metadata_set(struct sk_buff *skb, u8 meta_len) { }
static inline bool skb_csum_is_sctp(struct sk_buff *skb) { return false; }
static inline unsigned char *skb_checksum_start(const struct sk_buff *skb) { return skb->head + skb->csum_start; }
#ifndef prefetchw
#define prefetchw(x) __builtin_prefetch(x, 1)
#endif
static inline bool pcie_ptm_enabled(struct pci_dev *dev) { return false; }
#define netdev_err_once(dev, fmt, ...) netdev_err(dev, fmt, ##__VA_ARGS__)
static inline struct timespec64 timespec64_add(struct timespec64 a, struct timespec64 b)
{
    struct timespec64 r = { a.tv_sec + b.tv_sec, a.tv_nsec + b.tv_nsec };
    while (r.tv_nsec >= 1000000000L) {
        r.tv_sec++;
        r.tv_nsec -= 1000000000L;
    }
    return r;
}
static inline void timespec64_add_ns(struct timespec64 *a, u64 ns)
{
    u64 total = (u64)a->tv_nsec + ns;
    a->tv_sec += total / 1000000000ULL;
    a->tv_nsec = total % 1000000000ULL;
}
#include <linux/prefetch.h>
#define net_prefetch(p) prefetch(p)
#define net_prefetchw(p) prefetchw(p)
#ifndef NETIF_F_SCTP_CRC
#define NETIF_F_SCTP_CRC BIT_ULL(36)
#endif
static inline void page_ref_add(struct page *page, int nr) { atomic_add(nr, &page->refcount); }
static inline bool dev_page_is_reusable(const struct page *page) { return true; }
/* How much of a received frame to copy into the head: all of it (up to the
 * caller's limit) - packets are linear here. */
static inline u32 eth_get_headlen(const struct net_device *dev, const void *data, u32 len) { return len; }
static inline int __dev_uc_sync(struct net_device *dev, int (*sync)(struct net_device *, const unsigned char *),
                                int (*unsync)(struct net_device *, const unsigned char *))
{
    return 0;
}
/* Traffic classes (TSN offloads): one class. */
static inline void netdev_reset_tc(struct net_device *dev) { }
static inline int netdev_set_num_tc(struct net_device *dev, u8 num_tc) { return 0; }
static inline int netdev_set_tc_queue(struct net_device *dev, u8 tc, u16 count, u16 offset) { return 0; }

#include <net/lkpi_tc.h>
#ifndef NETIF_F_TSO_MANGLEID
#define NETIF_F_TSO_MANGLEID BIT_ULL(19)
#endif
static inline int netif_set_real_num_queues(struct net_device *dev, unsigned int txq, unsigned int rxq)
{
    int err = netif_set_real_num_tx_queues(dev, txq);
    if (!err)
        err = netif_set_real_num_rx_queues(dev, rxq);
    return err;
}

/* GSO feature bits Linux drivers name (unique here; GSO never reaches them). */
#ifndef NETIF_F_TSO_ECN
#define NETIF_F_TSO_ECN BIT_ULL(18)
#endif
#ifndef NETIF_F_GSO_GRE
#define NETIF_F_GSO_GRE BIT_ULL(52)
#define NETIF_F_GSO_GRE_CSUM BIT_ULL(53)
#define NETIF_F_GSO_IPXIP4 BIT_ULL(54)
#define NETIF_F_GSO_IPXIP6 BIT_ULL(55)
#define NETIF_F_GSO_UDP_TUNNEL BIT_ULL(56)
#define NETIF_F_GSO_UDP_TUNNEL_CSUM BIT_ULL(57)
#define NETIF_F_GSO_PARTIAL BIT_ULL(58)
#endif
#define TC_MQPRIO_HW_OFFLOAD_TCS 1
static inline int pci_enable_ptm(struct pci_dev *dev, u8 *granularity) { return -EINVAL; }
static inline bool napi_if_scheduled_mark_missed(struct napi_struct *n) { return false; }
static inline int timespec64_compare(const struct timespec64 *a, const struct timespec64 *b)
{
    if (a->tv_sec != b->tv_sec)
        return a->tv_sec < b->tv_sec ? -1 : 1;
    return a->tv_nsec < b->tv_nsec ? -1 : a->tv_nsec > b->tv_nsec;
}
static inline void pcie_print_link_status(struct pci_dev *dev) { }
static inline void pci_disable_ptm(struct pci_dev *dev) { }

/* (shared by several drivers) */
#define devm_kasprintf(dev, gfp, fmt, ...) kasprintf(gfp, fmt, ##__VA_ARGS__)
static inline bool pci_pme_capable(struct pci_dev *dev, int state) { return false; }
#ifndef NET_ADDR_RANDOM
#define NET_ADDR_PERM 0
#define NET_ADDR_RANDOM 1
#endif
static inline int eth_platform_get_mac_address(struct device *dev, u8 *mac_addr) { return -ENODEV; }
#define cpu_to_le16p(p) cpu_to_le16(*(p))
#define pci_err(pdev, fmt, ...) dev_err(&(pdev)->dev, fmt, ##__VA_ARGS__)
#define pci_warn(pdev, fmt, ...) dev_warn(&(pdev)->dev, fmt, ##__VA_ARGS__)
#define pci_info(pdev, fmt, ...) dev_info(&(pdev)->dev, fmt, ##__VA_ARGS__)
#define pci_dbg(pdev, fmt, ...) dev_dbg(&(pdev)->dev, fmt, ##__VA_ARGS__)
static inline int kstrtou8_from_user(const char __user *s, size_t count, unsigned int base, u8 *res) { return -EINVAL; }
#include <linux/platform_device.h>
/* Probe the platform devices present now (none here unless registered). */
static inline int platform_driver_probe(struct platform_driver *drv, int (*probe)(struct platform_device *))
{
    drv->probe = probe;
    return platform_driver_register(drv);
}

#include <linux/kthread.h>
#include <linux/of.h>

/* NAPI polls run as work items here anyway: "threaded" is only remembered. */
static inline int dev_set_threaded(struct net_device *dev, bool threaded)
{
    dev->threaded = threaded;
    return 0;
}
#ifndef L1_CACHE_ALIGN
#define L1_CACHE_ALIGN(x) ALIGN(x, 64)
#endif
/* 32-bit-word copies to/from device memory. */
static inline void __iowrite32_copy(void __iomem *to, const void *from, size_t count)
{
    volatile u32 __iomem *dst = to;
    const u32 *src = from;
    while (count--)
        *dst++ = *src++;
}
static inline void __ioread32_copy(void *to, const void __iomem *from, size_t count)
{
    u32 *dst = to;
    const volatile u32 __iomem *src = from;
    while (count--)
        *dst++ = *src++;
}

#ifndef ktime_get_raw_ts64
#define ktime_get_raw_ts64(ts) ktime_get_ts64(ts)
#endif
#ifndef NR_CPUS
#define NR_CPUS 256
#endif
#ifndef L1_CACHE_BYTES
#define L1_CACHE_BYTES 64
#endif
