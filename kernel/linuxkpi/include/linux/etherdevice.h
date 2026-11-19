#ifndef _LKPI_LINUX_ETHERDEVICE_H
#define _LKPI_LINUX_ETHERDEVICE_H

#include <linux/crc32.h>
#include <linux/netdevice.h>
#include <linux/random.h>
#include <linux/socket.h>

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
static inline u64 ether_addr_to_u64(const u8* addr) {
    u64 u = 0;
    for (int i = 0; i < ETH_ALEN; i++) u = u << 8 | addr[i];
    return u;
}
static inline void u64_to_ether_addr(u64 u, u8* addr) {
    for (int i = ETH_ALEN - 1; i >= 0; i--) { addr[i] = u & 0xff; u = u >> 8; }
}
static inline bool ether_addr_equal_unaligned(const u8* a, const u8* b) { return memcmp(a, b, ETH_ALEN) == 0; }
static inline bool ether_addr_equal_64bits(const u8* a, const u8* b) { return memcmp(a, b, ETH_ALEN) == 0; }
static inline bool ether_addr_equal_masked(const u8* a, const u8* b, const u8* mask) {
    for (int i = 0; i < ETH_ALEN; i++) if ((a[i] ^ b[i]) & mask[i]) return false;
    return true;
}
static inline bool is_local_ether_addr(const u8* a) { return a[0] & 0x02; }
static inline bool is_link_local_ether_addr(const u8* a) {
    static const u8 base[ETH_ALEN] = {0x01, 0x80, 0xc2, 0x00, 0x00, 0x00};
    return memcmp(a, base, 5) == 0 && (a[5] & 0xf0) == 0;
}
static inline void eth_addr_dec(u8* addr) { u64_to_ether_addr(ether_addr_to_u64(addr) - 1, addr); }
static inline void eth_addr_inc(u8* addr) { u64_to_ether_addr(ether_addr_to_u64(addr) + 1, addr); }
static inline void eth_hw_addr_inherit(struct net_device* dst, struct net_device* src) { eth_hw_addr_set(dst, src->dev_addr); }
static inline int eth_skb_pad(struct sk_buff* skb) { return skb_put_padto(skb, ETH_ZLEN); }
#define eth_header_ops_lkpi 0
static inline void eth_hw_addr_random(struct net_device* dev) {
    u8 a[ETH_ALEN];
    eth_random_addr(a);
    eth_hw_addr_set(dev, a);
}
int eth_validate_addr(struct net_device* dev);
int eth_mac_addr(struct net_device* dev, void* p);
#define eth_change_mtu NULL


LKPI_EXTERN_C_END

#endif
