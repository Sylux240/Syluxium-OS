#pragma once
#include <linux/skbuff.h>
int INET_ECN_set_ce(struct sk_buff* skb);
u8 skb_get_dsfield(struct sk_buff* skb);
#define INET_ECN_MASK 3
