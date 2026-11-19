/* LinuxKPI: kernel sockets are never handed to Linux code; only what the
 * wireless stack touches on skb->sk exists. */
#ifndef _LKPI_NET_SOCK_H
#define _LKPI_NET_SOCK_H
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <net/net_namespace.h>
struct sock {
    possible_net_t sk_net;
    int sk_protocol;
    u8 sk_pacing_shift;
    u32 sk_txhash;
    u32 sk_mark;
    unsigned long sk_flags;
    refcount_t sk_refcnt;
    u32 sk_tskey;
    u16 sk_tsflags;
};
static inline struct net* sock_net(const struct sock* sk) { return read_pnet(&sk->sk_net); }
static inline void sk_pacing_shift_update(struct sock* sk, int val) { if (sk && sk->sk_pacing_shift != val) sk->sk_pacing_shift = val; }
static inline void sock_hold(struct sock* sk) {}
static inline void sock_put(struct sock* sk) {}
static inline void sock_efree(struct sk_buff* skb) {}
static inline int sk_fullsock(const struct sock* sk) { return 1; }
#define SOCK_WIFI_STATUS 19
#define SOF_TIMESTAMPING_OPT_ID (1 << 7)
#define SOF_TIMESTAMPING_TX_HARDWARE (1 << 0)
static inline bool sock_flag(const struct sock* sk, int flag) { return false; }
static inline void skb_set_owner_w(struct sk_buff* skb, struct sock* sk) { skb->sk = sk; }
static inline int sock_queue_err_skb(struct sock* sk, struct sk_buff* skb) { kfree_skb(skb); return 0; }
#endif
