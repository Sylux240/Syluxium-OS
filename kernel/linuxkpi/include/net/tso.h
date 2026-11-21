/* LinuxKPI: TCP segmentation offload helpers (Linux net/tso.h). The
 * Syluxium network stack never hands drivers GSO packets, so drivers' TSO
 * paths are never taken; these keep them compiling. */
#ifndef _TSO_H
#define _TSO_H

#include <linux/skbuff.h>
#include <net/ip.h>

#define TSO_HEADER_SIZE 256

struct tso_t {
    int next_frag_idx;
    int size;
    void *data;
    u16 ip_id;
    u8 tlen;
    bool ipv6;
    u32 tcp_seq;
};

static inline int tso_count_descs(const struct sk_buff *skb)
{
    return skb_shinfo(skb)->gso_segs * 2 + skb_shinfo(skb)->nr_frags;
}

static inline void tso_build_hdr(const struct sk_buff *skb, char *hdr, struct tso_t *tso, int size, bool is_last)
{
    WARN_ON_ONCE(1);
}
static inline void tso_build_data(const struct sk_buff *skb, struct tso_t *tso, int size) { WARN_ON_ONCE(1); }
static inline int tso_start(struct sk_buff *skb, struct tso_t *tso)
{
    WARN_ON_ONCE(1);
    return 0;
}

#endif
