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

/* System timestamps around a PHC read (PTP: not configured). */
struct ptp_system_timestamp;
static inline void ptp_read_system_prets(struct ptp_system_timestamp *sts) { }
static inline void ptp_read_system_postts(struct ptp_system_timestamp *sts) { }

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
