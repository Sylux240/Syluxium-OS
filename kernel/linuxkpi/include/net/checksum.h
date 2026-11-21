/* LinuxKPI: checksum helpers (net/checksum.h). Checksum offload of TSO
 * packets only: drivers here never get those, so the values are not used. */
#pragma once
#include <linux/lkpi_all.h>
static inline __sum16 csum_tcpudp_magic(__be32 saddr, __be32 daddr, u32 len, u8 proto, __wsum sum) { return 0; }
