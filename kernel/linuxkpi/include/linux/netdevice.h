/* LinuxKPI: network devices. A registered net_device becomes a Syluxium
 * network interface (eth0, wlan0, ...) through the glue in glue/netdev.c:
 * the stack's outgoing frames go to ndo_start_xmit(), frames the driver
 * passes to netif_receive_skb()/netif_rx() go up the Syluxium stack, and
 * "ip link set X up/down" calls dev_open()/dev_close(). */
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
#include <linux/notifier.h>
#include <linux/u64_stats_sync.h>
#include <uapi/linux/if.h>

LKPI_EXTERN_C_BEGIN

struct net;
struct wireless_dev;
struct netlink_ext_ack;
struct ifreq;
struct net_device;
struct netdev_queue;
struct sockaddr;

typedef u64 netdev_features_t;
#define NETIF_F_SG BIT_ULL(0)
#define NETIF_F_IP_CSUM BIT_ULL(1)
#define NETIF_F_HW_CSUM BIT_ULL(3)
#define NETIF_F_IPV6_CSUM BIT_ULL(4)
#define NETIF_F_HIGHDMA BIT_ULL(5)
#define NETIF_F_HW_VLAN_CTAG_TX BIT_ULL(7)
#define NETIF_F_HW_VLAN_CTAG_RX BIT_ULL(8)
#define NETIF_F_TSO BIT_ULL(16)
#define NETIF_F_TSO6 BIT_ULL(20)
#define NETIF_F_RXCSUM BIT_ULL(29)
#define NETIF_F_RXFCS BIT_ULL(30)
#define NETIF_F_RXALL BIT_ULL(31)
#define NETIF_F_HW_TC BIT_ULL(48)
#define NETIF_F_LLTX BIT_ULL(12)
#define NETIF_F_GSO_SOFTWARE (NETIF_F_TSO | NETIF_F_TSO6)
#define NETIF_F_CSUM_MASK (NETIF_F_IP_CSUM | NETIF_F_IPV6_CSUM | NETIF_F_HW_CSUM)

typedef enum netdev_tx {
    __NETDEV_TX_MIN = -2147483647 - 1,
    NETDEV_TX_OK = 0x00,
    NETDEV_TX_BUSY = 0x10,
} netdev_tx_t;

/* Private flags (dev->priv_flags). */
#define NET_RX_SUCCESS 0
#define NET_RX_DROP 1
#define NET_XMIT_SUCCESS 0
#define NET_XMIT_DROP 1
#define IFF_802_1Q_VLAN (1ULL << 0)
#define IFF_EBRIDGE (1ULL << 1)
#define IFF_BONDING (1ULL << 2)
#define IFF_ISATAP (1ULL << 3)
#define IFF_WAN_HDLC (1ULL << 4)
#define IFF_XMIT_DST_RELEASE (1ULL << 5)
#define IFF_DONT_BRIDGE (1ULL << 6)
#define IFF_DISABLE_NETPOLL (1ULL << 7)
#define IFF_MACVLAN_PORT (1ULL << 8)
#define IFF_BRIDGE_PORT (1ULL << 9)
#define IFF_OVS_DATAPATH (1ULL << 10)
#define IFF_TX_SKB_SHARING (1ULL << 11)
#define IFF_UNICAST_FLT (1ULL << 12)
#define IFF_TEAM_PORT (1ULL << 13)
#define IFF_SUPP_NOFCS (1ULL << 14)
#define IFF_LIVE_ADDR_CHANGE (1ULL << 15)
#define IFF_MACVLAN (1ULL << 16)
#define IFF_XMIT_DST_RELEASE_PERM (1ULL << 17)
#define IFF_L3MDEV_MASTER (1ULL << 18)
#define IFF_NO_QUEUE (1ULL << 19)
#define IFF_OPENVSWITCH (1ULL << 20)
#define IFF_L3MDEV_SLAVE (1ULL << 21)
#define IFF_TEAM (1ULL << 22)
#define IFF_RXFH_CONFIGURED (1ULL << 23)
#define IFF_PHONY_HEADROOM (1ULL << 24)
#define IFF_MACSEC (1ULL << 25)
#define IFF_NO_RX_HANDLER (1ULL << 26)
#define IFF_FAILOVER (1ULL << 27)
#define IFF_FAILOVER_SLAVE (1ULL << 28)
#define IFF_L3MDEV_RX_HANDLER (1ULL << 29)
#define IFF_NO_ADDRCONF (1ULL << 30)
#define IFF_TX_SKB_NO_LINEAR (1ULL << 31)

#define NET_NAME_UNKNOWN 0
#define NET_NAME_ENUM 1
#define NET_NAME_PREDICTABLE 2
#define NET_NAME_USER 3
#define NET_NAME_RENAMED 4

#define MAX_ADDR_LEN 32
#define LL_MAX_HEADER 128
#define LL_RESERVED_SPACE(dev) ((((dev)->hard_header_len + (dev)->needed_headroom) & ~(16 - 1)) + 16)

/* Netdevice notifier events. */
enum netdev_cmd {
    NETDEV_UP = 1,
    NETDEV_DOWN,
    NETDEV_REBOOT,
    NETDEV_CHANGE,
    NETDEV_REGISTER,
    NETDEV_UNREGISTER,
    NETDEV_CHANGEMTU,
    NETDEV_CHANGEADDR,
    NETDEV_PRE_CHANGEADDR,
    NETDEV_GOING_DOWN,
    NETDEV_CHANGENAME,
    NETDEV_FEAT_CHANGE,
    NETDEV_BONDING_FAILOVER,
    NETDEV_PRE_UP,
    NETDEV_PRE_TYPE_CHANGE,
    NETDEV_POST_TYPE_CHANGE,
    NETDEV_POST_INIT,
    NETDEV_PRE_UNINIT,
    NETDEV_RELEASE,
    NETDEV_NOTIFY_PEERS,
    NETDEV_JOIN,
    NETDEV_CHANGEUPPER,
    NETDEV_RESEND_IGMP,
    NETDEV_PRECHANGEMTU,
    NETDEV_CHANGEINFODATA,
    NETDEV_BONDING_INFO,
    NETDEV_PRECHANGEUPPER,
    NETDEV_CHANGELOWERSTATE,
    NETDEV_UDP_TUNNEL_PUSH_INFO,
    NETDEV_UDP_TUNNEL_DROP_INFO,
    NETDEV_CHANGE_TX_QUEUE_LEN,
    NETDEV_CVLAN_FILTER_PUSH_INFO,
    NETDEV_CVLAN_FILTER_DROP_INFO,
    NETDEV_SVLAN_FILTER_PUSH_INFO,
    NETDEV_SVLAN_FILTER_DROP_INFO,
    NETDEV_OFFLOAD_XSTATS_ENABLE,
    NETDEV_OFFLOAD_XSTATS_DISABLE,
    NETDEV_OFFLOAD_XSTATS_REPORT_USED,
    NETDEV_OFFLOAD_XSTATS_REPORT_DELTA,
    NETDEV_XDP_FEAT_CHANGE,
};

struct netdev_notifier_info {
    struct net_device* dev;
    struct netlink_ext_ack* extack;
};
static inline struct net_device* netdev_notifier_info_to_dev(const struct netdev_notifier_info* info) { return info->dev; }
int register_netdevice_notifier(struct notifier_block* nb);
int unregister_netdevice_notifier(struct notifier_block* nb);
int call_netdevice_notifiers(unsigned long val, struct net_device* dev);
void netdev_state_change(struct net_device* dev);
void netdev_features_change(struct net_device* dev);
#define netdev_notify_peers(dev) do { } while (0)

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
    u64 rx_compressed, tx_compressed, rx_nohandler, rx_otherhost_dropped;
};

static inline void netdev_stats_to_stats64(struct rtnl_link_stats64* s64, const struct net_device_stats* s) {
    const unsigned long* src = (const unsigned long*)s;
    u64* dst = (u64*)s64;
    size_t n = sizeof(*s) / sizeof(unsigned long);
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
}

/* Per-CPU software statistics (one instance here). */
struct pcpu_sw_netstats {
    u64_stats_t rx_packets, rx_bytes, tx_packets, tx_bytes;
    struct u64_stats_sync syncp;
};
enum netdev_stat_type {
    NETDEV_PCPU_STAT_NONE,
    NETDEV_PCPU_STAT_LSTATS,
    NETDEV_PCPU_STAT_TSTATS,
    NETDEV_PCPU_STAT_DSTATS,
};
void dev_sw_netstats_rx_add(struct net_device* dev, unsigned int len);
void dev_sw_netstats_tx_add(struct net_device* dev, unsigned int packets, unsigned int len);
void dev_fetch_sw_netstats(struct rtnl_link_stats64* s, const struct pcpu_sw_netstats* netstats);
void dev_get_tstats64(struct net_device* dev, struct rtnl_link_stats64* s);

enum tc_setup_type { TC_SETUP_QDISC_MQPRIO, TC_QUERY_CAPS };
enum net_device_path_type {
    DEV_PATH_ETHERNET = 0, DEV_PATH_VLAN, DEV_PATH_BRIDGE, DEV_PATH_PPPOE, DEV_PATH_DSA, DEV_PATH_MTK_WDMA,
};
struct net_device_path {
    enum net_device_path_type type;
    const struct net_device* dev;
    union {
        struct { u16 id; __be16 proto; u8 h_dest[ETH_ALEN]; } encap;
        struct { u8 wdma_idx; u8 queue; u16 wcid; u8 bss; u8 amsdu; } mtk_wdma;
    };
};
#define NET_DEVICE_PATH_STACK_MAX 5
#define NET_DEVICE_PATH_VLAN_MAX 2
struct net_device_path_ctx {
    const struct net_device* dev;
    u8 daddr[ETH_ALEN];
    int num_vlans;
    struct { u16 id; __be16 proto; } vlan[NET_DEVICE_PATH_VLAN_MAX];
};

struct net_device_ops {
    int (*ndo_init)(struct net_device* dev);
    void (*ndo_uninit)(struct net_device* dev);
    int (*ndo_open)(struct net_device* dev);
    int (*ndo_stop)(struct net_device* dev);
    netdev_tx_t (*ndo_start_xmit)(struct sk_buff* skb, struct net_device* dev);
    netdev_features_t (*ndo_features_check)(struct sk_buff* skb, struct net_device* dev, netdev_features_t features);
    u16 (*ndo_select_queue)(struct net_device* dev, struct sk_buff* skb, struct net_device* sb_dev);
    void (*ndo_change_rx_flags)(struct net_device* dev, int flags);
    void (*ndo_set_rx_mode)(struct net_device* dev);
    int (*ndo_set_mac_address)(struct net_device* dev, void* addr);
    int (*ndo_validate_addr)(struct net_device* dev);
    int (*ndo_do_ioctl)(struct net_device* dev, struct ifreq* ifr, int cmd);
    int (*ndo_eth_ioctl)(struct net_device* dev, struct ifreq* ifr, int cmd);
    int (*ndo_siocdevprivate)(struct net_device* dev, struct ifreq* ifr, void __user* data, int cmd);
    int (*ndo_change_mtu)(struct net_device* dev, int new_mtu);
    void (*ndo_tx_timeout)(struct net_device* dev, unsigned int txqueue);
    void (*ndo_get_stats64)(struct net_device* dev, struct rtnl_link_stats64* storage);
    struct net_device_stats* (*ndo_get_stats)(struct net_device* dev);
    void (*ndo_poll_controller)(struct net_device* dev);
    int (*ndo_vlan_rx_add_vid)(struct net_device* dev, __be16 proto, u16 vid);
    int (*ndo_vlan_rx_kill_vid)(struct net_device* dev, __be16 proto, u16 vid);
    int (*ndo_setup_tc)(struct net_device* dev, enum tc_setup_type type, void* type_data);
    netdev_features_t (*ndo_fix_features)(struct net_device* dev, netdev_features_t features);
    int (*ndo_set_features)(struct net_device* dev, netdev_features_t features);
    int (*ndo_fill_forward_path)(struct net_device_path_ctx* ctx, struct net_device_path* path);
};

/* Hardware address lists (multicast filters). */
struct netdev_hw_addr {
    struct list_head list;
    unsigned char addr[MAX_ADDR_LEN];
    unsigned char type;
    bool global_use;
    int sync_cnt;
    int refcount;
    int synced;
    struct rcu_head rcu_head;
};
struct netdev_hw_addr_list {
    struct list_head list;
    int count;
};
#define netdev_hw_addr_list_count(l) ((l)->count)
#define netdev_hw_addr_list_empty(l) (netdev_hw_addr_list_count(l) == 0)
#define netdev_hw_addr_list_for_each(ha, l) list_for_each_entry(ha, &(l)->list, list)
#define netdev_mc_count(dev) netdev_hw_addr_list_count(&(dev)->mc)
#define netdev_mc_empty(dev) netdev_hw_addr_list_empty(&(dev)->mc)
#define netdev_for_each_mc_addr(ha, dev) netdev_hw_addr_list_for_each(ha, &(dev)->mc)
#define netdev_uc_count(dev) netdev_hw_addr_list_count(&(dev)->uc)
#define netdev_uc_empty(dev) netdev_hw_addr_list_empty(&(dev)->uc)
#define netdev_for_each_uc_addr(ha, dev) netdev_hw_addr_list_for_each(ha, &(dev)->uc)
int __hw_addr_sync(struct netdev_hw_addr_list* to_list, struct netdev_hw_addr_list* from_list, int addr_len);
void __hw_addr_unsync(struct netdev_hw_addr_list* to_list, struct netdev_hw_addr_list* from_list, int addr_len);
void __hw_addr_init(struct netdev_hw_addr_list* list);
static inline void netif_addr_lock_bh(struct net_device* dev) {}
static inline void netif_addr_unlock_bh(struct net_device* dev) {}
static inline void netif_addr_lock(struct net_device* dev) {}
static inline void netif_addr_unlock(struct net_device* dev) {}

struct napi_struct {
    struct net_device* dev;
    int (*poll)(struct napi_struct* n, int budget);
    int weight;
    volatile unsigned long state;
    struct work_struct lkpi_work;
};
#define NAPI_POLL_WEIGHT 64

/* Transmit queue state. */
enum netdev_queue_state_t { __QUEUE_STATE_DRV_XOFF, __QUEUE_STATE_STACK_XOFF, __QUEUE_STATE_FROZEN };
struct netdev_queue {
    struct net_device* dev;
    unsigned long state;
    unsigned long trans_start;
};

struct header_ops;

struct net_device {
    char name[IFNAMSIZ];
    unsigned long state;
    struct list_head dev_list;
    netdev_features_t features, hw_features, wanted_features, vlan_features, hw_enc_features, mpls_features, gso_partial_features;
    unsigned int flags;
    unsigned long long priv_flags;
    const struct net_device_ops* netdev_ops;
    const struct header_ops* header_ops;
    unsigned short gflags;
    unsigned short needed_headroom;
    unsigned short needed_tailroom;
    unsigned int mtu, min_mtu, max_mtu;
    unsigned short type;
    unsigned short hard_header_len;
    unsigned char min_header_len;
    unsigned char name_assign_type;
    unsigned char if_port;
    unsigned char dma;
    unsigned char addr_len;
    unsigned char addr_assign_type;
    unsigned char perm_addr[MAX_ADDR_LEN];
    unsigned char broadcast[MAX_ADDR_LEN];
    const unsigned char* dev_addr;
    struct netdev_hw_addr_list uc, mc, dev_addrs;
    int ifindex;
    int irq;
    unsigned long base_addr;
    int watchdog_timeo;
    unsigned int tx_queue_len;
    unsigned int num_tx_queues, real_num_tx_queues;
    unsigned int num_rx_queues, real_num_rx_queues;
    struct netdev_queue* _tx;
    struct net_device_stats stats;
    atomic_long_t rx_dropped, tx_dropped;
    enum netdev_stat_type pcpu_stat_type : 8;
    union {
        struct pcpu_sw_netstats __percpu* tstats;
        void* lstats;
    };
    const struct ethtool_ops* ethtool_ops;
    struct wireless_dev* ieee80211_ptr;
    struct device dev;
    const struct attribute_group* sysfs_groups[4];
    bool needs_free_netdev;
    void (*priv_destructor)(struct net_device* dev);
    bool netns_local;
    bool lltx;
    u8 reg_state;
    u16 group;
    unsigned char operstate;
    unsigned char link_mode;
    int ml_priv_type;
    void* ml_priv;
    struct net* nd_net;
    refcount_t dev_refcnt;
    /* LinuxKPI private */
    unsigned char lkpi_addr[MAX_ADDR_LEN];
    volatile int lkpi_carrier;
    int lkpi_running;
    int lkpi_auto_open;     /* Ethernet drivers: open at registration (Syluxium interfaces start up) */
    void* lkpi_iface;       /* Syluxium net::Interface */
    size_t lkpi_priv_size;
    /* phylib (added before the private area, which must stay last) */
    struct phy_link_topology* link_topo;
    struct phy_device* phydev;
    struct sfp_bus* sfp_bus;
    struct ethtool_netdev_state* ethtool;
    unsigned long lkpi_priv[] __aligned(64);
};

#define NETREG_UNINITIALIZED 0
#define NETREG_REGISTERED 1
#define NETREG_UNREGISTERING 2
#define NETREG_UNREGISTERED 3

/* dev->state bits */
enum netdev_state_t {
    __LINK_STATE_START,
    __LINK_STATE_PRESENT,
    __LINK_STATE_NOCARRIER,
    __LINK_STATE_LINKWATCH_PENDING,
    __LINK_STATE_DORMANT,
    __LINK_STATE_TESTING,
};

static inline void* netdev_priv(const struct net_device* dev) { return (void*)dev->lkpi_priv; }
#define SET_NETDEV_DEV(net, pdev) ((net)->dev.parent = (pdev))
#define SET_NETDEV_DEVTYPE(net, devtype) do { (void)(net); } while (0)
#define NETDEV_ALIGN 32
#define to_net_dev(d) container_of(d, struct net_device, dev)

struct net_device* alloc_netdev_mqs(int sizeof_priv, const char* name, unsigned char name_assign_type,
                                    void (*setup)(struct net_device*), unsigned int txqs, unsigned int rxqs);
#define alloc_netdev(sizeof_priv, name, name_assign_type, setup) alloc_netdev_mqs(sizeof_priv, name, name_assign_type, setup, 1, 1)
struct net_device* alloc_etherdev_mqs(int sizeof_priv, unsigned int txqs, unsigned int rxqs);
#define alloc_etherdev(sizeof_priv) alloc_etherdev_mqs(sizeof_priv, 1, 1)
#define alloc_etherdev_mq(sizeof_priv, q) alloc_etherdev_mqs(sizeof_priv, q, q)
#define devm_alloc_etherdev(dev, sizeof_priv) alloc_etherdev(sizeof_priv)
void ether_setup(struct net_device* dev);
void free_netdev(struct net_device* dev);
int register_netdev(struct net_device* dev);
int register_netdevice(struct net_device* dev);
void unregister_netdev(struct net_device* dev);
void unregister_netdevice_queue(struct net_device* dev, struct list_head* head);
static inline void unregister_netdevice(struct net_device* dev) { unregister_netdevice_queue(dev, NULL); }
void unregister_netdevice_many(struct list_head* head);
#define devm_register_netdev(d, n) register_netdev(n)
int dev_alloc_name(struct net_device* dev, const char* name);
int dev_open(struct net_device* dev, struct netlink_ext_ack* extack);
void dev_close(struct net_device* dev);
void dev_close_many(struct list_head* head, bool unlink);
int dev_change_net_namespace(struct net_device* dev, struct net* net, const char* pat);
struct net_device* dev_get_by_index(struct net* net, int ifindex);
struct net_device* __dev_get_by_index(struct net* net, int ifindex);
struct net_device* dev_get_by_index_rcu(struct net* net, int ifindex);
struct net_device* dev_get_by_name(struct net* net, const char* name);
struct net_device* __dev_get_by_name(struct net* net, const char* name);
int dev_queue_xmit(struct sk_buff* skb);
#define __dev_queue_xmit(skb, sb_dev) dev_queue_xmit(skb)
int dev_set_mtu(struct net_device* dev, int mtu);
void dev_set_uevent_suppress_lkpi(struct device* dev, int val);
static inline void dev_hold(struct net_device* dev) { if (dev) refcount_inc(&dev->dev_refcnt); }
static inline void dev_put(struct net_device* dev) { if (dev) refcount_dec(&dev->dev_refcnt); }
typedef struct { int unused; } netdevice_tracker;
#define netdev_hold(dev, tracker, gfp) dev_hold(dev)
#define netdev_put(dev, tracker) dev_put(dev)
#define netdev_tracker_alloc(dev, tracker, gfp) do { } while (0)
#define netdev_tracker_free(dev, tracker) do { } while (0)
static inline struct net* dev_net(const struct net_device* dev);
#define dev_net_set(dev, net) do { (dev)->nd_net = (net); } while (0)
static inline void eth_hw_addr_set(struct net_device* dev, const u8* addr) {
    memcpy(dev->lkpi_addr, addr, ETH_ALEN);
    dev->dev_addr = dev->lkpi_addr;
}
static inline void dev_addr_set(struct net_device* dev, const u8* addr) { memcpy(dev->lkpi_addr, addr, dev->addr_len); dev->dev_addr = dev->lkpi_addr; }
static inline void __dev_addr_set(struct net_device* dev, const void* addr, size_t len) { memcpy(dev->lkpi_addr, addr, len); dev->dev_addr = dev->lkpi_addr; }
static inline void dev_addr_mod(struct net_device* dev, unsigned int offset, const void* addr, size_t len) { memcpy(dev->lkpi_addr + offset, addr, len); }
int dev_set_mac_address(struct net_device* dev, struct sockaddr* sa, struct netlink_ext_ack* extack);
static inline bool netif_is_bridge_port(const struct net_device* dev) { return false; }
static inline bool netif_is_macsec(const struct net_device* dev) { return false; }
static inline bool netif_is_l3_master(const struct net_device* dev) { return false; }
static inline void netdev_set_default_ethtool_ops(struct net_device* dev, const struct ethtool_ops* ops) { if (!dev->ethtool_ops) dev->ethtool_ops = ops; }
static inline void netif_keep_dst(struct net_device* dev) {}
static inline bool netif_reduces_vlan_mtu(struct net_device* dev) { return false; }
static inline bool netif_device_present(const struct net_device* dev) { return true; }

/* Transmit queues. */
static inline struct netdev_queue* netdev_get_tx_queue(const struct net_device* dev, unsigned int index) { return &dev->_tx[index]; }
void netif_tx_wake_queue(struct netdev_queue* q);
static inline void netif_tx_start_queue(struct netdev_queue* q) { clear_bit(__QUEUE_STATE_DRV_XOFF, &q->state); }
static inline void netif_tx_stop_queue(struct netdev_queue* q) { set_bit(__QUEUE_STATE_DRV_XOFF, &q->state); }
static inline bool netif_tx_queue_stopped(const struct netdev_queue* q) { return test_bit(__QUEUE_STATE_DRV_XOFF, &q->state); }
static inline bool netif_xmit_stopped(const struct netdev_queue* q) { return netif_tx_queue_stopped(q); }
static inline void netif_wake_queue(struct net_device* dev) { netif_tx_wake_queue(netdev_get_tx_queue(dev, 0)); }
static inline void netif_start_queue(struct net_device* dev) { netif_tx_start_queue(netdev_get_tx_queue(dev, 0)); }
static inline void netif_stop_queue(struct net_device* dev) { netif_tx_stop_queue(netdev_get_tx_queue(dev, 0)); }
static inline bool netif_queue_stopped(const struct net_device* dev) { return netif_tx_queue_stopped(netdev_get_tx_queue(dev, 0)); }
static inline void netif_tx_start_all_queues(struct net_device* dev) { for (unsigned int i = 0; i < dev->num_tx_queues; i++) netif_tx_start_queue(netdev_get_tx_queue(dev, i)); }
static inline void netif_tx_wake_all_queues(struct net_device* dev) { for (unsigned int i = 0; i < dev->num_tx_queues; i++) netif_tx_wake_queue(netdev_get_tx_queue(dev, i)); }
static inline void netif_tx_stop_all_queues(struct net_device* dev) { for (unsigned int i = 0; i < dev->num_tx_queues; i++) netif_tx_stop_queue(netdev_get_tx_queue(dev, i)); }
#define netif_tx_disable netif_tx_stop_all_queues
static inline void netif_stop_subqueue(struct net_device* dev, u16 queue_index) { netif_tx_stop_queue(netdev_get_tx_queue(dev, queue_index)); }
static inline void netif_wake_subqueue(struct net_device* dev, u16 queue_index) { netif_tx_wake_queue(netdev_get_tx_queue(dev, queue_index)); }
static inline void netif_start_subqueue(struct net_device* dev, u16 queue_index) { netif_tx_start_queue(netdev_get_tx_queue(dev, queue_index)); }
static inline bool __netif_subqueue_stopped(const struct net_device* dev, u16 queue_index) { return netif_tx_queue_stopped(netdev_get_tx_queue(dev, queue_index)); }
static inline bool netif_subqueue_stopped(const struct net_device* dev, struct sk_buff* skb) { return __netif_subqueue_stopped(dev, skb_get_queue_mapping(skb)); }
static inline int netif_set_real_num_tx_queues(struct net_device* dev, unsigned int n) { dev->real_num_tx_queues = n; return 0; }
static inline int netif_set_real_num_rx_queues(struct net_device* dev, unsigned int n) { dev->real_num_rx_queues = n; return 0; }
static inline void netif_tx_lock_bh(struct net_device* dev) {}
static inline void netif_tx_unlock_bh(struct net_device* dev) {}
static inline void netif_tx_lock(struct net_device* dev) {}
static inline void netif_tx_unlock(struct net_device* dev) {}
static inline void __netif_tx_lock(struct netdev_queue* txq, int cpu) {}
static inline void __netif_tx_unlock(struct netdev_queue* txq) {}
static inline void txq_trans_cond_update(struct netdev_queue* txq) {}
static inline int netif_running(const struct net_device* dev) { return test_bit(__LINK_STATE_START, &dev->state); }
void netif_carrier_on(struct net_device* dev);
void netif_carrier_off(struct net_device* dev);
static inline int netif_carrier_ok(const struct net_device* dev) { return dev->lkpi_carrier; }
static inline void netif_device_detach(struct net_device* dev) { netif_tx_stop_all_queues(dev); }
static inline void netif_device_attach(struct net_device* dev) { netif_tx_wake_all_queues(dev); }
static inline void netif_trans_update(struct net_device* dev) {}
static inline void netdev_sent_queue(struct net_device* dev, unsigned int bytes) {}
static inline void netdev_completed_queue(struct net_device* dev, unsigned int pkts, unsigned int bytes) {}
static inline void netdev_reset_queue(struct net_device* dev) {}
static inline void netif_dormant_on(struct net_device* dev) {}
static inline void netif_dormant_off(struct net_device* dev) {}
static inline bool netif_oper_up(const struct net_device* dev) { return netif_running(dev) && netif_carrier_ok(dev); }
void netdev_update_features(struct net_device* dev);
static inline netdev_features_t vlan_features_check(struct sk_buff* skb, netdev_features_t f) { return f; }
static inline netdev_features_t passthru_features_check(struct sk_buff* skb, struct net_device* dev, netdev_features_t f) { return f; }

/* Receive. */
int netif_receive_skb(struct sk_buff* skb);
int netif_rx(struct sk_buff* skb);
#define netif_rx_ni netif_rx
#define __netif_rx netif_rx
void netif_receive_skb_list(struct list_head* head);
#define napi_gro_receive(napi, skb) netif_receive_skb(skb)
__be16 eth_type_trans(struct sk_buff* skb, struct net_device* dev);

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
static inline bool napi_schedule(struct napi_struct* n) { if (napi_schedule_prep(n)) { __napi_schedule(n); return true; } return false; }
#define napi_schedule_irqoff napi_schedule
bool napi_complete_done(struct napi_struct* napi, int work_done);
#define napi_complete(n) napi_complete_done(n, 0)

/* rtnl lock (one global mutex). */
void rtnl_lock(void);
void rtnl_unlock(void);
int rtnl_trylock(void);
int rtnl_is_locked(void);
#define ASSERT_RTNL() do { } while (0)
#define rtnl_dereference(p) (p)
#define rcu_dereference_rtnl(p) (p)
#define rtnl_net_dereference(net, p) (p)
extern const int net_ns_type_operations;
#define rtnl_net_lock(net) rtnl_lock()
#define rtnl_net_unlock(net) rtnl_unlock()

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
#define net_ratelimit() 1
#define net_dbg_ratelimited(fmt, ...) do { if (0) printk(fmt, ##__VA_ARGS__); } while (0)
#define net_info_ratelimited(fmt, ...) pr_info(fmt, ##__VA_ARGS__)
#define net_warn_ratelimited(fmt, ...) pr_warn(fmt, ##__VA_ARGS__)
#define net_err_ratelimited(fmt, ...) pr_err(fmt, ##__VA_ARGS__)
static inline const char* netdev_name(const struct net_device* dev) { return dev->name; }

LKPI_EXTERN_C_END

#include <net/net_namespace.h>
#include <net/sock.h>
static inline struct net* dev_net(const struct net_device* dev) { return dev->nd_net ? dev->nd_net : &init_net; }

#endif
