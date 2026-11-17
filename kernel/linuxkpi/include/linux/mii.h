#ifndef _LKPI_LINUX_MII_H
#define _LKPI_LINUX_MII_H

#include <linux/ethtool.h>
#include <linux/kernel.h>

LKPI_EXTERN_C_BEGIN

/* Generic MII registers (IEEE 802.3 clause 22). */
#define MII_BMCR 0x00
#define MII_BMSR 0x01
#define MII_PHYSID1 0x02
#define MII_PHYSID2 0x03
#define MII_ADVERTISE 0x04
#define MII_LPA 0x05
#define MII_EXPANSION 0x06
#define MII_CTRL1000 0x09
#define MII_STAT1000 0x0a

#define BMCR_RESV 0x003f
#define BMCR_SPEED1000 0x0040
#define BMCR_CTST 0x0080
#define BMCR_FULLDPLX 0x0100
#define BMCR_ANRESTART 0x0200
#define BMCR_ISOLATE 0x0400
#define BMCR_PDOWN 0x0800
#define BMCR_ANENABLE 0x1000
#define BMCR_SPEED100 0x2000
#define BMCR_LOOPBACK 0x4000
#define BMCR_RESET 0x8000

#define BMSR_ERCAP 0x0001
#define BMSR_JCD 0x0002
#define BMSR_LSTATUS 0x0004
#define BMSR_ANEGCAPABLE 0x0008
#define BMSR_RFAULT 0x0010
#define BMSR_ANEGCOMPLETE 0x0020
#define BMSR_10HALF 0x0800
#define BMSR_10FULL 0x1000
#define BMSR_100HALF 0x2000
#define BMSR_100FULL 0x4000

#define ADVERTISE_SLCT 0x001f
#define ADVERTISE_CSMA 0x0001
#define ADVERTISE_10HALF 0x0020
#define ADVERTISE_10FULL 0x0040
#define ADVERTISE_100HALF 0x0080
#define ADVERTISE_100FULL 0x0100
#define ADVERTISE_100BASE4 0x0200
#define ADVERTISE_PAUSE_CAP 0x0400
#define ADVERTISE_PAUSE_ASYM 0x0800
#define ADVERTISE_LPACK 0x4000
#define ADVERTISE_NPAGE 0x8000
#define ADVERTISE_FULL (ADVERTISE_100FULL | ADVERTISE_10FULL | ADVERTISE_CSMA)
#define ADVERTISE_ALL (ADVERTISE_10HALF | ADVERTISE_10FULL | ADVERTISE_100HALF | ADVERTISE_100FULL)

#define LPA_SLCT 0x001f
#define LPA_10HALF 0x0020
#define LPA_10FULL 0x0040
#define LPA_100HALF 0x0080
#define LPA_100FULL 0x0100
#define LPA_100BASE4 0x0200
#define LPA_PAUSE_CAP 0x0400
#define LPA_PAUSE_ASYM 0x0800
#define LPA_LPACK 0x4000
#define LPA_DUPLEX (LPA_10FULL | LPA_100FULL)
#define LPA_100 (LPA_100FULL | LPA_100HALF | LPA_100BASE4)

struct net_device;

struct mii_if_info {
    int phy_id;
    int advertising;
    int phy_id_mask;
    int reg_num_mask;
    unsigned int full_duplex : 1;
    unsigned int force_media : 1;
    unsigned int supports_gmii : 1;
    struct net_device* dev;
    int (*mdio_read)(struct net_device* dev, int phy_id, int location);
    void (*mdio_write)(struct net_device* dev, int phy_id, int location, int val);
};

struct mii_ioctl_data {
    u16 phy_id, reg_num, val_in, val_out;
};

int mii_link_ok(struct mii_if_info* mii);
int mii_nway_restart(struct mii_if_info* mii);
unsigned int mii_check_media(struct mii_if_info* mii, unsigned int ok_to_print, unsigned int init_media);
void mii_check_link(struct mii_if_info* mii);
void mii_ethtool_get_link_ksettings(struct mii_if_info* mii, struct ethtool_link_ksettings* cmd);
int mii_ethtool_set_link_ksettings(struct mii_if_info* mii, const struct ethtool_link_ksettings* cmd);
struct ifreq;
int generic_mii_ioctl(struct mii_if_info* mii, struct mii_ioctl_data* data, int cmd, unsigned int* duplex_changed);
static inline struct mii_ioctl_data* if_mii(struct ifreq* rq) { return (struct mii_ioctl_data*)((char*)rq + 16); }
static inline unsigned int mii_nway_result(unsigned int negotiated) {
    if (negotiated & LPA_100FULL) return LPA_100FULL;
    if (negotiated & LPA_100BASE4) return LPA_100BASE4;
    if (negotiated & LPA_100HALF) return LPA_100HALF;
    if (negotiated & LPA_10FULL) return LPA_10FULL;
    return LPA_10HALF;
}
static inline unsigned int mii_duplex(unsigned int duplex_lock, unsigned int negotiated) {
    if (duplex_lock) return 1;
    return mii_nway_result(negotiated) & LPA_DUPLEX ? 1 : 0;
}

LKPI_EXTERN_C_END

#endif
