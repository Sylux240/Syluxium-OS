/* LinuxKPI: packet drop reasons (registered, never reported). */
#ifndef _LKPI_NET_DROPREASON_H
#define _LKPI_NET_DROPREASON_H

#include <linux/skbuff.h>

enum skb_drop_reason_subsys {
    SKB_DROP_REASON_SUBSYS_CORE,
    SKB_DROP_REASON_SUBSYS_MAC80211_UNUSABLE,
    SKB_DROP_REASON_SUBSYS_MAC80211_MONITOR,
    SKB_DROP_REASON_SUBSYS_OPENVSWITCH,
    SKB_DROP_REASON_SUBSYS_NUM
};

struct drop_reason_list {
    const char* const* reasons;
    size_t n_reasons;
};

static inline void drop_reasons_register_subsys(enum skb_drop_reason_subsys subsys, const struct drop_reason_list* list) {}
static inline void drop_reasons_unregister_subsys(enum skb_drop_reason_subsys subsys) {}

#endif
