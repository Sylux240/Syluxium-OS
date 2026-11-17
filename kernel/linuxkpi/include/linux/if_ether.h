#ifndef _LKPI_LINUX_IF_ETHER_H
#define _LKPI_LINUX_IF_ETHER_H

#include <linux/types.h>

#define ETH_ALEN 6
#define ETH_HLEN 14
#define ETH_ZLEN 60
#define ETH_DATA_LEN 1500
#define ETH_FRAME_LEN 1514
#define ETH_FCS_LEN 4
#define ETH_MIN_MTU 68
#define ETH_MAX_MTU 0xFFFFU
#define ETH_P_IP 0x0800
#define ETH_P_ARP 0x0806
#define ETH_P_8021Q 0x8100
#define ETH_P_IPV6 0x86DD
#define ETH_P_802_3_MIN 0x0600
#define ETH_P_802_2 0x0004
#define VLAN_HLEN 4
#define VLAN_ETH_HLEN 18
#define VLAN_ETH_FRAME_LEN 1518

struct ethhdr {
    unsigned char h_dest[ETH_ALEN];
    unsigned char h_source[ETH_ALEN];
    __be16 h_proto;
} __attribute__((packed));

#endif
