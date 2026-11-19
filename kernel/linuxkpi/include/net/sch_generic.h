#pragma once
#include <linux/netdevice.h>
static inline bool qdisc_all_tx_empty(const struct net_device* dev) { return true; }
#define TC_PRIO_CONTROL 7
