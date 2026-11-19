#ifndef _LKPI_LINUX_IP_H
#define _LKPI_LINUX_IP_H
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/skbuff.h>
struct iphdr {
    u8 ihl : 4, version : 4;
    u8 tos;
    __be16 tot_len, id, frag_off;
    u8 ttl, protocol;
    __sum16 check;
    __be32 saddr, daddr;
};
struct ipv6hdr {
    u8 priority : 4, version : 4;
    u8 flow_lbl[3];
    __be16 payload_len;
    u8 nexthdr;
    u8 hop_limit;
    struct in6_addr saddr, daddr;
};
static inline struct iphdr* ip_hdr(const struct sk_buff* skb) { return (struct iphdr*)skb_network_header(skb); }
static inline struct ipv6hdr* ipv6_hdr(const struct sk_buff* skb) { return (struct ipv6hdr*)skb_network_header(skb); }
static inline unsigned int ip_hdrlen(const struct sk_buff* skb) { return ip_hdr(skb)->ihl * 4; }
#define IP_OFFSET 0x1FFF
#define IP_MF 0x2000
#define IP_DF 0x4000
#endif
