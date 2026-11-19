#pragma once
#include <linux/kernel.h>
#include <linux/ip.h>
u8 skb_get_dsfield(struct sk_buff* skb);
static inline u8 ipv4_get_dsfield(const struct iphdr* iph) { return iph->tos; }
static inline u8 ipv6_get_dsfield(const struct ipv6hdr* ipv6h) { return (u8)(ntohs(*(const __be16*)ipv6h) >> 4); }
