/* LinuxKPI: XDP tracepoints (none). */
#pragma once
#include <net/xdp.h>
static inline void trace_xdp_exception(const struct net_device *dev, const struct bpf_prog *prog, u32 act) { }
