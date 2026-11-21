#ifndef _LKPI_LINUX_UDP_H
#define _LKPI_LINUX_UDP_H
#include <linux/ip.h>
struct udphdr {
    __be16 source, dest, len;
    __sum16 check;
};
static inline struct udphdr *udp_hdr(const struct sk_buff *skb) { return (struct udphdr *)skb_transport_header(skb); }
#endif
