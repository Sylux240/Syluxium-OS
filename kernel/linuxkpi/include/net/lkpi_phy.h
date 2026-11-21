/* LinuxKPI: what phylib and the Ethernet drivers (r8169, e1000e; Linux
 * 6.12) expect from the kernel headers beyond the common LinuxKPI set.
 * Included first (Makefile). */
#pragma once

#include <linux/lkpi_all.h>
#include <linux/device/bus.h>
#include <linux/devres.h>
#include <linux/ethtool.h>
#include <linux/linkmode.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <net/lkpi_netcompat.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/gpio/consumer.h>

struct clk;
struct reset_control;
#ifndef reset_control_put
static inline void reset_control_put(struct reset_control *r) { }
#endif

#define MDIO_NAME_SIZE 32
struct mdio_device_id {
    __u32 phy_id;
    __u32 phy_id_mask;
};

/* No device tree on x86 PCs: nothing to read from it. */
struct device_node {
    struct fwnode_handle fwnode;
    const char *name;
};
#define FWNODE_FLAG_NEEDS_CHILD_BOUND_ON_ADD (1 << 2)
#define of_fwnode_handle(np) ((np) ? &(np)->fwnode : NULL)
static inline struct device_node *of_get_child_by_name(const struct device_node *np, const char *name) { return NULL; }
#define for_each_available_child_of_node_scoped(parent, child) \
    for (struct device_node *child = NULL; child != NULL;)
static inline struct device *bus_find_device_by_fwnode(const struct bus_type *bus, const struct fwnode_handle *f)
{
    return NULL;
}
static inline struct fwnode_handle *fwnode_find_reference(const struct fwnode_handle *f, const char *name,
                                                          unsigned int index)
{
    return ERR_PTR(-ENOENT);
}
static inline bool is_acpi_node(const struct fwnode_handle *f) { return false; }
static inline int fwnode_property_read_string(const struct fwnode_handle *f, const char *name, const char **val)
{
    return -EINVAL;
}
static inline struct device *class_find_device_by_name(const void *cls, const char *name) { return NULL; }
static inline int clk_prepare_enable(struct clk *c) { return 0; }

/* LED class registration with init data (phylib's PHY LEDs from the device tree). */
struct led_init_data {
    struct fwnode_handle *fwnode;
    const char *default_label;
    const char *devicename;
    bool devname_mandatory;
};
#define led_classdev_register_ext(parent, cdev, init) (-ENOSYS)
#define devm_led_classdev_register_ext(parent, cdev, init) (-ENOSYS)

/* MII ioctls (linux/sockios.h). */
#ifndef SIOCGMIIPHY
#define SIOCGMIIPHY 0x8947
#define SIOCGMIIREG 0x8948
#define SIOCSMIIREG 0x8949
#endif
#ifndef SIOCSHWTSTAMP
#define SIOCSHWTSTAMP 0x89b0
#define SIOCGHWTSTAMP 0x89b1
#endif
static inline bool of_property_read_bool(const struct device_node *np, const char *name) { return false; }
static inline struct device_node *of_get_parent(const struct device_node *np) { return NULL; }
static inline bool of_node_name_eq(const struct device_node *np, const char *name) { return false; }

/* Reset lines and clocks of PHYs on SoC boards: none here. */
struct reset_control;
static inline struct reset_control *reset_control_get_optional_exclusive(struct device *dev, const char *id)
{
    return NULL;
}
struct clk;
static inline struct clk *devm_clk_get_optional_enabled(struct device *dev, const char *id) { return NULL; }
#ifndef clk_disable_unprepare
static inline void clk_disable_unprepare(struct clk *c) { }
#endif
static inline int gpiod_set_consumer_name(struct gpio_desc *desc, const char *name) { return 0; }

static inline void irq_wake_thread(unsigned int irq, void *dev_id) { }
static inline void pm_wakeup_dev_event(struct device *dev, unsigned int msec, bool hard) { }
static inline void pm_system_wakeup(void) { }
static inline int sysfs_create_link_nowarn(struct kobject *kobj, struct kobject *target, const char *name) { return 0; }

/* Self-test state of a link (ethtool -t): never entered here. */
static inline void netif_testing_on(struct net_device *dev) { }
static inline void netif_testing_off(struct net_device *dev) { }

/* LED triggers of network devices (linux/leds.h). */
enum led_trigger_netdev_modes {
    TRIGGER_NETDEV_LINK = 0,
    TRIGGER_NETDEV_LINK_10,
    TRIGGER_NETDEV_LINK_100,
    TRIGGER_NETDEV_LINK_1000,
    TRIGGER_NETDEV_LINK_2500,
    TRIGGER_NETDEV_LINK_5000,
    TRIGGER_NETDEV_LINK_10000,
    TRIGGER_NETDEV_HALF_DUPLEX,
    TRIGGER_NETDEV_FULL_DUPLEX,
    TRIGGER_NETDEV_TX,
    TRIGGER_NETDEV_RX,
    TRIGGER_NETDEV_TX_ERR,
    TRIGGER_NETDEV_RX_ERR,
    __TRIGGER_NETDEV_MAX,
};

/* xa_insert: store only where nothing is stored yet. */
static inline int xa_insert(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp)
{
    if (xa_load(xa, index))
        return -EBUSY;
    xa_store(xa, index, entry, gfp);
    return 0;
}

/* ---- r8169: network helpers ---- */
#define PCIBIOS_SUCCESSFUL 0x00
#ifndef IPPROTO_RAW
#define IPPROTO_RAW 255
#endif
#define NETIF_F_ALL_TSO (NETIF_F_TSO | NETIF_F_TSO6)
#define netdev_notice_once(dev, fmt, ...) netdev_notice(dev, fmt, ##__VA_ARGS__)
#define vlan_get_protocol(skb) ((skb)->protocol)
#define eth_hw_addr_crc(ha) ether_crc(ETH_ALEN, (ha)->addr)

#include <linux/tcp.h>
#include <linux/udp.h>

/* GSO packets never reach drivers here. */
static inline void tcp_v6_gso_csum_prep(struct sk_buff *skb) { }

/* Byte queue limits: not used; every packet rings the doorbell. */
static inline bool netdev_xmit_more(void) { return false; }
static inline void netdev_tx_completed_queue(struct netdev_queue *q, unsigned int pkts, unsigned int bytes) { }
static inline void netdev_tx_sent_queue(struct netdev_queue *q, unsigned int bytes) { }
static inline bool __netdev_sent_queue(struct net_device *dev, unsigned int bytes, bool xmit_more) { return true; }
static inline void netdev_tx_reset_queue(struct netdev_queue *q) { }

#define IRQF_NO_THREAD 0
#define netdev_warn_once(dev, fmt, ...) netdev_warn(dev, fmt, ##__VA_ARGS__)
static inline void netif_set_tso_max_size(struct net_device *dev, unsigned int size) { }
static inline void netif_set_tso_max_segs(struct net_device *dev, unsigned int segs) { }
/* Power-off paths (WoL at shutdown): never taken here. */
enum system_states { SYSTEM_BOOTING, SYSTEM_SCHEDULING, SYSTEM_FREEING_INITMEM, SYSTEM_RUNNING, SYSTEM_HALT,
                     SYSTEM_POWER_OFF, SYSTEM_RESTART, SYSTEM_SUSPEND };
#define system_state SYSTEM_RUNNING
/* Runtime PM: devices stay powered. */
static inline int pm_schedule_suspend(struct device *dev, unsigned int delay) { return 0; }
static inline int pm_request_resume(struct device *dev) { return 0; }
static inline int pcim_set_mwi(struct pci_dev *dev) { return 0; }
static inline int pci_reset_bus(struct pci_dev *dev) { return -ENOTTY; }
static inline u16 pci_status_get_and_clear_errors(struct pci_dev *pdev) { return 0; }
static inline void netdev_sw_irq_coalesce_default_on(struct net_device *dev) { }
static inline int pci_select_bars(struct pci_dev *dev, unsigned long flags)
{
    int bars = 0;
    for (int i = 0; i < 6; i++)
        if (dev->lkpi_res_len[i] && (dev->lkpi_res_flags[i] & flags))
            bars |= 1 << i;
    return bars;
}

/* Interface statistics through the driver's ndo_get_stats64. */
static inline struct rtnl_link_stats64 *dev_get_stats(struct net_device *dev, struct rtnl_link_stats64 *storage)
{
    memset(storage, 0, sizeof(*storage));
    if (dev->netdev_ops && dev->netdev_ops->ndo_get_stats64)
        dev->netdev_ops->ndo_get_stats64(dev, storage);
    return storage;
}
