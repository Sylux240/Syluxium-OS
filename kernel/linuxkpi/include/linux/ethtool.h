#ifndef _LKPI_LINUX_ETHTOOL_H
#define _LKPI_LINUX_ETHTOOL_H

#include <linux/kernel.h>

LKPI_EXTERN_C_BEGIN

/* ethtool: the structures drivers fill in. Syluxium doesn't expose the
 * ethtool ioctl yet; the operations are kept for the future Device Manager. */
#define ETH_GSTRING_LEN 32
#define ETHTOOL_FWVERS_LEN 32
#define ETHTOOL_BUSINFO_LEN 32
enum ethtool_stringset { ETH_SS_TEST = 0, ETH_SS_STATS = 1, ETH_SS_PRIV_FLAGS = 2 };

#define WAKE_PHY (1 << 0)
#define WAKE_UCAST (1 << 1)
#define WAKE_MCAST (1 << 2)
#define WAKE_BCAST (1 << 3)
#define WAKE_ARP (1 << 4)
#define WAKE_MAGIC (1 << 5)
#define WAKE_MAGICSECURE (1 << 6)

#define SPEED_10 10
#define SPEED_100 100
#define SPEED_1000 1000
#define SPEED_UNKNOWN -1
#define DUPLEX_HALF 0x00
#define DUPLEX_FULL 0x01
#define DUPLEX_UNKNOWN 0xff
#define PORT_TP 0x00
#define PORT_MII 0x02
#define XCVR_INTERNAL 0x00
#define XCVR_EXTERNAL 0x01
#define AUTONEG_DISABLE 0x00
#define AUTONEG_ENABLE 0x01

struct ethtool_drvinfo {
    u32 cmd;
    char driver[32];
    char version[32];
    char fw_version[ETHTOOL_FWVERS_LEN];
    char bus_info[ETHTOOL_BUSINFO_LEN];
    char erom_version[32];
    char reserved2[12];
    u32 n_priv_flags, n_stats, testinfo_len, eedump_len, regdump_len;
};
struct ethtool_wolinfo {
    u32 cmd;
    u32 supported;
    u32 wolopts;
    u8 sopass[6];
};
struct ethtool_regs {
    u32 cmd;
    u32 version;
    u32 len;
    u8 data[];
};
struct ethtool_stats {
    u32 cmd;
    u32 n_stats;
    u64 data[];
};
struct ethtool_link_settings {
    u32 cmd, speed;
    u8 duplex, port, phy_address, autoneg, mdio_support, eth_tp_mdix, eth_tp_mdix_ctrl;
    s8 link_mode_masks_nwords;
    u8 transceiver, master_slave_cfg, master_slave_state, rate_matching;
    u32 reserved[7];
};
struct ethtool_link_ksettings {
    struct ethtool_link_settings base;
    struct {
        unsigned long supported[2], advertising[2], lp_advertising[2];
    } link_modes;
};
struct ethtool_ringparam {
    u32 cmd, rx_max_pending, rx_mini_max_pending, rx_jumbo_max_pending, tx_max_pending;
    u32 rx_pending, rx_mini_pending, rx_jumbo_pending, tx_pending;
};
struct ethtool_pauseparam {
    u32 cmd, autoneg, rx_pause, tx_pause;
};
struct ethtool_eeprom {
    u32 cmd, magic, offset, len;
    u8 data[];
};
struct ethtool_coalesce;
struct kernel_ethtool_ringparam;
struct netlink_ext_ack;
struct net_device;

struct ethtool_ops {
    u32 supported_coalesce_params;
    void (*get_drvinfo)(struct net_device*, struct ethtool_drvinfo*);
    int (*get_regs_len)(struct net_device*);
    void (*get_regs)(struct net_device*, struct ethtool_regs*, void*);
    void (*get_wol)(struct net_device*, struct ethtool_wolinfo*);
    int (*set_wol)(struct net_device*, struct ethtool_wolinfo*);
    u32 (*get_msglevel)(struct net_device*);
    void (*set_msglevel)(struct net_device*, u32);
    int (*nway_reset)(struct net_device*);
    u32 (*get_link)(struct net_device*);
    int (*get_eeprom_len)(struct net_device*);
    int (*get_eeprom)(struct net_device*, struct ethtool_eeprom*, u8*);
    int (*set_eeprom)(struct net_device*, struct ethtool_eeprom*, u8*);
    void (*get_ringparam)(struct net_device*, struct ethtool_ringparam*, struct kernel_ethtool_ringparam*, struct netlink_ext_ack*);
    void (*get_pauseparam)(struct net_device*, struct ethtool_pauseparam*);
    int (*set_pauseparam)(struct net_device*, struct ethtool_pauseparam*);
    void (*get_strings)(struct net_device*, u32 stringset, u8*);
    int (*get_sset_count)(struct net_device*, int);
    void (*get_ethtool_stats)(struct net_device*, struct ethtool_stats*, u64*);
    int (*get_link_ksettings)(struct net_device*, struct ethtool_link_ksettings*);
    int (*set_link_ksettings)(struct net_device*, const struct ethtool_link_ksettings*);
    int (*get_ts_info)(struct net_device*, void*);
};
u32 ethtool_op_get_link(struct net_device* dev);
#define ethtool_op_get_ts_info NULL

LKPI_EXTERN_C_END

#endif
