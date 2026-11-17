#ifndef _LKPI_LINUX_IP_H
#define _LKPI_LINUX_IP_H
#include <linux/in.h>
#include <linux/skbuff.h>
struct iphdr {
    u8 ihl : 4, version : 4;
    u8 tos;
    __be16 tot_len, id, frag_off;
    u8 ttl, protocol;
    __sum16 check;
    __be32 saddr, daddr;
};
static inline struct iphdr* ip_hdr(const struct sk_buff* skb) { return (struct iphdr*)(skb->mac_header_ptr ? skb->mac_header_ptr + 14 : skb->data + 14); }
#endif
