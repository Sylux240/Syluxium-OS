#ifndef _LKPI_LINUX_ETHERDEVICE_H
#define _LKPI_LINUX_ETHERDEVICE_H

#include <linux/crc32.h>
#include <linux/netdevice.h>
#include <linux/random.h>

LKPI_EXTERN_C_BEGIN

static inline bool is_zero_ether_addr(const u8* a) { return !(a[0] | a[1] | a[2] | a[3] | a[4] | a[5]); }
static inline bool is_multicast_ether_addr(const u8* a) { return a[0] & 1; }
static inline bool is_broadcast_ether_addr(const u8* a) { return (a[0] & a[1] & a[2] & a[3] & a[4] & a[5]) == 0xff; }
static inline bool is_unicast_ether_addr(const u8* a) { return !is_multicast_ether_addr(a); }
static inline bool is_valid_ether_addr(const u8* a) { return !is_multicast_ether_addr(a) && !is_zero_ether_addr(a); }
static inline bool ether_addr_equal(const u8* a, const u8* b) { return memcmp(a, b, ETH_ALEN) == 0; }
static inline void ether_addr_copy(u8* dst, const u8* src) { memcpy(dst, src, ETH_ALEN); }
static inline void eth_broadcast_addr(u8* a) { memset(a, 0xff, ETH_ALEN); }
static inline void eth_zero_addr(u8* a) { memset(a, 0, ETH_ALEN); }
void eth_random_addr(u8* addr);
static inline void eth_hw_addr_random(struct net_device* dev) {
    u8 a[ETH_ALEN];
    eth_random_addr(a);
    eth_hw_addr_set(dev, a);
}
int eth_validate_addr(struct net_device* dev);
int eth_mac_addr(struct net_device* dev, void* p);
#define eth_change_mtu NULL

/* struct sockaddr for ndo_set_mac_address. */
struct sockaddr {
    unsigned short sa_family;
    char sa_data[14];
};

LKPI_EXTERN_C_END

#endif
