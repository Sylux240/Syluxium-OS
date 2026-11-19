#ifndef _LKPI_LINUX_IF_VLAN_H
#define _LKPI_LINUX_IF_VLAN_H
#include <linux/netdevice.h>
#define VLAN_HLEN 4
#define VLAN_ETH_HLEN 18
#define VLAN_ETH_ZLEN 64
#define VLAN_ETH_DATA_LEN 1500
#define VLAN_ETH_FRAME_LEN 1518
#define VLAN_PRIO_MASK 0xe000
#define VLAN_PRIO_SHIFT 13
#define VLAN_CFI_MASK 0x1000
#define VLAN_VID_MASK 0x0fff
#define VLAN_N_VID 4096
struct vlan_hdr { __be16 h_vlan_TCI; __be16 h_vlan_encapsulated_proto; };
struct vlan_ethhdr { unsigned char h_dest[ETH_ALEN]; unsigned char h_source[ETH_ALEN]; __be16 h_vlan_proto; __be16 h_vlan_TCI; __be16 h_vlan_encapsulated_proto; };
static inline bool is_vlan_dev(const struct net_device* dev) { return false; }
static inline bool eth_type_vlan(__be16 ethertype) { return ethertype == htons(ETH_P_8021Q) || ethertype == htons(ETH_P_8021AD); }
#endif
