#ifndef _LKPI_LINUX_NETDEVICE_H
#define _LKPI_LINUX_NETDEVICE_H

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/ethtool.h>
#include <linux/if_ether.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

LKPI_EXTERN_C_BEGIN

typedef u64 netdev_features_t;
#define NETIF_F_SG BIT_ULL(0)
#define NETIF_F_IP_CSUM BIT_ULL(1)
#define NETIF_F_HW_CSUM BIT_ULL(3)
#define NETIF_F_IPV6_CSUM BIT_ULL(4)
#define NETIF_F_HIGHDMA BIT_ULL(5)
#define NETIF_F_HW_VLAN_CTAG_TX BIT_ULL(7)
#define NETIF_F_HW_VLAN_CTAG_RX BIT_ULL(8)
#define NETIF_F_RXCSUM BIT_ULL(29)
#define NETIF_F_RXFCS BIT_ULL(30)
#define NETIF_F_RXALL BIT_ULL(31)
#define NETIF_F_TSO BIT_ULL(16)
#define NETIF_F_TSO6 BIT_ULL(20)

typedef enum netdev_tx {
    NETDEV_TX_OK = 0x00,
    NETDEV_TX_BUSY = 0x10,
} netdev_tx_t;

/* Interface flags (also in <linux/if.h>). */
#define IFF_UP 0x1
#define IFF_BROADCAST 0x2
#define IFF_DEBUG 0x4
#define IFF_LOOPBACK 0x8
#define IFF_RUNNING 0x40
#define IFF_PROMISC 0x100
#define IFF_ALLMULTI 0x200
#define IFF_MULTICAST 0x1000
#define IFNAMSIZ 16

struct net_device_stats {
    unsigned long rx_packets, tx_packets, rx_bytes, tx_bytes;
    unsigned long rx_errors, tx_errors, rx_dropped, tx_dropped, multicast, collisions;
    unsigned long rx_length_errors, rx_over_errors, rx_crc_errors, rx_frame_errors, rx_fifo_errors, rx_missed_errors;
    unsigned long tx_aborted_errors, tx_carrier_errors, tx_fifo_errors, tx_heartbeat_errors, tx_window_errors;
    unsigned long rx_compressed, tx_compressed;
};
struct rtnl_link_stats64 {
    u64 rx_packets, tx_packets, rx_bytes, tx_bytes, rx_errors, tx_errors, rx_dropped, tx_dropped, multicast, collisions;
    u64 rx_length_errors, rx_over_errors, rx_crc_errors, rx_frame_errors, rx_fifo_errors, rx_missed_errors;
    u64 tx_aborted_errors, tx_carrier_errors, tx_fifo_errors, tx_heartbeat_errors, tx_window_errors;
};

struct ifreq;
struct net_device;

static inline void netdev_stats_to_stats64(struct rtnl_link_stats64* s64, const struct net_device_stats* s) {
    const unsigned long* src = (const unsigned long*)s;
    u64* dst = (u64*)s64;
    size_t n = sizeof(*s64) / sizeof(u64) < sizeof(*s) / sizeof(unsigned long) ? sizeof(*s64) / sizeof(u64)
                                                                               : sizeof(*s) / sizeof(unsigned long);
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
}

struct net_device_ops {
    int (*ndo_init)(struct net_device* dev);
    void (*ndo_uninit)(struct net_device* dev);
    int (*ndo_open)(struct net_device* dev);
    int (*ndo_stop)(struct net_device* dev);
    netdev_tx_t (*ndo_start_xmit)(struct sk_buff* skb, struct net_device* dev);
    void (*ndo_set_rx_mode)(struct net_device* dev);
    int (*ndo_set_mac_address)(struct net_device* dev, void* addr);
    int (*ndo_validate_addr)(struct net_device* dev);
    int (*ndo_eth_ioctl)(struct net_device* dev, struct ifreq* ifr, int cmd);
    int (*ndo_change_mtu)(struct net_device* dev, int new_mtu);
    void (*ndo_tx_timeout)(struct net_device* dev, unsigned int txqueue);
    struct net_device_stats* (*ndo_get_stats)(struct net_device* dev);
    void (*ndo_get_stats64)(struct net_device* dev, struct rtnl_link_stats64* s);
    void (*ndo_poll_controller)(struct net_device* dev);
    int (*ndo_set_features)(struct net_device* dev, netdev_features_t features);
    netdev_features_t (*ndo_fix_features)(struct net_device* dev, netdev_features_t features);
    netdev_features_t (*ndo_features_check)(struct sk_buff* skb, struct net_device* dev, netdev_features_t features);
    int (*ndo_vlan_rx_add_vid)(struct net_device* dev, u16 proto, u16 vid);
    int (*ndo_vlan_rx_kill_vid)(struct net_device* dev, u16 proto, u16 vid);
};

/* Multicast list entry (no multicast filtering yet: the list is always empty). */
struct netdev_hw_addr {
    struct list_head list;
    unsigned char addr[32];
};

struct napi_struct {
    struct net_device* dev;
    int (*poll)(struct napi_struct* n, int budget);
    int weight;
    volatile unsigned long state;
    struct work_struct lkpi_work;
};
#define NAPI_POLL_WEIGHT 64

struct net_device {
    char name[IFNAMSIZ];
    struct device dev;
    unsigned long state;
    unsigned int flags;
    unsigned int priv_flags;
    unsigned short type;
    unsigned short hard_header_len;
    unsigned int mtu, min_mtu, max_mtu;
    netdev_features_t features, hw_features, vlan_features, wanted_features;
    const unsigned char* dev_addr;
    unsigned char perm_addr[32];
    unsigned char broadcast[32];
    unsigned char addr_len;
    unsigned char if_port;
    int irq;
    unsigned long base_addr;
    int watchdog_timeo;
    unsigned int tx_queue_len;
    struct net_device_stats stats;
    const struct net_device_ops* netdev_ops;
    const struct ethtool_ops* ethtool_ops;
    int ifindex;
    /* LinuxKPI private */
    unsigned char lkpi_addr[32];
    volatile int lkpi_queue_stopped;
    volatile int lkpi_carrier;
    int lkpi_running;
    void* lkpi_iface;      /* Syluxium net::Interface */
    size_t lkpi_priv_size;
    unsigned long lkpi_priv[] __aligned(64);
};

static inline void* netdev_priv(const struct net_device* dev) { return (void*)dev->lkpi_priv; }
#define SET_NETDEV_DEV(net, pdev) ((net)->dev.parent = (pdev))

struct net_device* alloc_etherdev_mqs(int sizeof_priv, unsigned int txqs, unsigned int rxqs);
#define alloc_etherdev(sizeof_priv) alloc_etherdev_mqs(sizeof_priv, 1, 1)
#define alloc_etherdev_mq(sizeof_priv, q) alloc_etherdev_mqs(sizeof_priv, q, q)
#define devm_alloc_etherdev(dev, sizeof_priv) alloc_etherdev(sizeof_priv)
void free_netdev(struct net_device* dev);
int register_netdev(struct net_device* dev);
#define register_netdevice register_netdev
void unregister_netdev(struct net_device* dev);
#define unregister_netdevice unregister_netdev
#define devm_register_netdev(d, n) register_netdev(n)
static inline void eth_hw_addr_set(struct net_device* dev, const u8* addr) {
    memcpy(dev->lkpi_addr, addr, ETH_ALEN);
    dev->dev_addr = dev->lkpi_addr;
}
#define dev_addr_set(dev, addr) eth_hw_addr_set(dev, addr)
static inline void __dev_addr_set(struct net_device* dev, const void* addr, size_t len) { memcpy(dev->lkpi_addr, addr, len); dev->dev_addr = dev->lkpi_addr; }

/* Transmit queue state. */
void netif_wake_queue(struct net_device* dev);
static inline void netif_start_queue(struct net_device* dev) { dev->lkpi_queue_stopped = 0; }
static inline void netif_stop_queue(struct net_device* dev) { dev->lkpi_queue_stopped = 1; }
static inline int netif_queue_stopped(const struct net_device* dev) { return dev->lkpi_queue_stopped; }
#define netif_tx_start_all_queues netif_start_queue
#define netif_tx_wake_all_queues netif_wake_queue
#define netif_tx_stop_all_queues netif_stop_queue
#define netif_tx_disable netif_stop_queue
static inline int netif_running(const struct net_device* dev) { return dev->lkpi_running; }
void netif_carrier_on(struct net_device* dev);
void netif_carrier_off(struct net_device* dev);
static inline int netif_carrier_ok(const struct net_device* dev) { return dev->lkpi_carrier; }
static inline void netif_device_detach(struct net_device* dev) { netif_stop_queue(dev); }
static inline void netif_device_attach(struct net_device* dev) { netif_wake_queue(dev); }
static inline int netif_device_present(const struct net_device* dev) { return 1; }
static inline void netif_trans_update(struct net_device* dev) {}
static inline void netdev_sent_queue(struct net_device* dev, unsigned int bytes) {}
static inline void netdev_completed_queue(struct net_device* dev, unsigned int pkts, unsigned int bytes) {}
static inline void netdev_reset_queue(struct net_device* dev) {}
#define netdev_mc_count(dev) 0
#define netdev_mc_empty(dev) 1
#define netdev_uc_count(dev) 0
#define netdev_for_each_mc_addr(ha, dev) for ((ha) = NULL; 0;)
#define netdev_for_each_uc_addr(ha, dev) for ((ha) = NULL; 0;)
void netdev_update_features(struct net_device* dev);
static inline netdev_features_t vlan_features_check(struct sk_buff* skb, netdev_features_t f) { return f; }

/* Receive. */
int netif_receive_skb(struct sk_buff* skb);
#define netif_rx netif_receive_skb
#define netif_rx_ni netif_receive_skb
#define napi_gro_receive(napi, skb) netif_receive_skb(skb)
u16 eth_type_trans(struct sk_buff* skb, struct net_device* dev);

/* NAPI: poll functions run on a LinuxKPI worker thread. */
void netif_napi_add(struct net_device* dev, struct napi_struct* napi, int (*poll)(struct napi_struct*, int));
#define netif_napi_add_weight(dev, napi, poll, w) netif_napi_add(dev, napi, poll)
#define netif_napi_add_tx(dev, napi, poll) netif_napi_add(dev, napi, poll)
void netif_napi_del(struct napi_struct* napi);
void napi_enable(struct napi_struct* napi);
void napi_disable(struct napi_struct* napi);
bool napi_schedule_prep(struct napi_struct* napi);
void __napi_schedule(struct napi_struct* napi);
#define __napi_schedule_irqoff __napi_schedule
static inline void napi_schedule(struct napi_struct* n) { if (napi_schedule_prep(n)) __napi_schedule(n); }
#define napi_schedule_irqoff napi_schedule
bool napi_complete_done(struct napi_struct* napi, int work_done);
#define napi_complete(n) napi_complete_done(n, 0)

/* rtnl lock (one global mutex). */
void rtnl_lock(void);
void rtnl_unlock(void);
#define ASSERT_RTNL() do { } while (0)

/* Message levels (ethtool msglvl). */
#define NETIF_MSG_DRV 0x0001
#define NETIF_MSG_PROBE 0x0002
#define NETIF_MSG_LINK 0x0004
#define NETIF_MSG_TIMER 0x0008
#define NETIF_MSG_IFDOWN 0x0010
#define NETIF_MSG_IFUP 0x0020
#define NETIF_MSG_RX_ERR 0x0040
#define NETIF_MSG_TX_ERR 0x0080
#define NETIF_MSG_TX_QUEUED 0x0100
#define NETIF_MSG_INTR 0x0200
#define NETIF_MSG_TX_DONE 0x0400
#define NETIF_MSG_RX_STATUS 0x0800
#define NETIF_MSG_PKTDATA 0x1000
#define NETIF_MSG_HW 0x2000
#define NETIF_MSG_WOL 0x4000
#define netif_msg_drv(p) ((p)->msg_enable & NETIF_MSG_DRV)
#define netif_msg_probe(p) ((p)->msg_enable & NETIF_MSG_PROBE)
#define netif_msg_link(p) ((p)->msg_enable & NETIF_MSG_LINK)
#define netif_msg_timer(p) ((p)->msg_enable & NETIF_MSG_TIMER)
#define netif_msg_ifdown(p) ((p)->msg_enable & NETIF_MSG_IFDOWN)
#define netif_msg_ifup(p) ((p)->msg_enable & NETIF_MSG_IFUP)
#define netif_msg_rx_err(p) ((p)->msg_enable & NETIF_MSG_RX_ERR)
#define netif_msg_tx_err(p) ((p)->msg_enable & NETIF_MSG_TX_ERR)
#define netif_msg_intr(p) ((p)->msg_enable & NETIF_MSG_INTR)
#define netif_msg_pktdata(p) ((p)->msg_enable & NETIF_MSG_PKTDATA)
#define netif_msg_hw(p) ((p)->msg_enable & NETIF_MSG_HW)
static inline u32 netif_msg_init(int debug_value, int default_msg) {
    if (debug_value < 0 || debug_value >= 32) return default_msg;
    if (debug_value == 0) return 0;
    return (1U << debug_value) - 1;
}

int lkpi_netdev_printk(const char* level, const struct net_device* dev, const char* fmt, ...) __printf(3, 4);
#define netdev_err(d, fmt, ...) lkpi_netdev_printk(KERN_ERR, d, fmt, ##__VA_ARGS__)
#define netdev_warn(d, fmt, ...) lkpi_netdev_printk(KERN_WARNING, d, fmt, ##__VA_ARGS__)
#define netdev_notice(d, fmt, ...) lkpi_netdev_printk(KERN_NOTICE, d, fmt, ##__VA_ARGS__)
#define netdev_info(d, fmt, ...) lkpi_netdev_printk(KERN_INFO, d, fmt, ##__VA_ARGS__)
#define netdev_dbg(d, fmt, ...) do { if (0) lkpi_netdev_printk(KERN_DEBUG, d, fmt, ##__VA_ARGS__); } while (0)
#define netif_err(p, type, d, fmt, ...) do { if (netif_msg_##type(p)) netdev_err(d, fmt, ##__VA_ARGS__); } while (0)
#define netif_warn(p, type, d, fmt, ...) do { if (netif_msg_##type(p)) netdev_warn(d, fmt, ##__VA_ARGS__); } while (0)
#define netif_info(p, type, d, fmt, ...) do { if (netif_msg_##type(p)) netdev_info(d, fmt, ##__VA_ARGS__); } while (0)
#define netdev_WARN(d, fmt, ...) netdev_warn(d, fmt, ##__VA_ARGS__)
#define netdev_WARN_ONCE(d, fmt, ...) netdev_warn(d, fmt, ##__VA_ARGS__)
#define netif_dbg(p, type, d, fmt, ...) do { if (0) netdev_dbg(d, fmt, ##__VA_ARGS__); } while (0)

/* 64-bit statistics: plain counters on 64-bit CPUs. */
struct u64_stats_sync { int unused; };
#define u64_stats_init(s) do { (void)(s); } while (0)
#define u64_stats_update_begin(s) do { (void)(s); } while (0)
#define u64_stats_update_end(s) do { (void)(s); } while (0)
#define u64_stats_fetch_begin(s) ((void)(s), 0u)
#define u64_stats_fetch_retry(s, start) ((void)(s), (void)(start), 0)
typedef struct { u64 v; } u64_stats_t;
#define u64_stats_read(p) ((p)->v)
#define u64_stats_set(p, val) ((p)->v = (val))
#define u64_stats_add(p, val) ((p)->v += (val))
#define u64_stats_inc(p) ((p)->v++)

LKPI_EXTERN_C_END

#endif
