/* LinuxKPI: tc classifier helpers for drivers (Linux 7.4
 * include/net/pkt_cls.h). No tc in Syluxium: see net/flow_offload.h. */
#pragma once
#include <net/flow_offload.h>

static inline bool tc_cls_can_offload_and_chain0(const struct net_device *dev, struct flow_cls_common_offload *common)
{
	return false;
}
static inline int tc_classid_to_hwtc(struct net_device *dev, u32 classid)
{
	return -1;
}
