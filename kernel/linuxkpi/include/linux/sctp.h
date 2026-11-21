/* LinuxKPI: the SCTP header (checksum offload of SCTP packets). */
#pragma once
#include <linux/lkpi_all.h>
struct sctphdr {
    __be16 source, dest;
    __be32 vtag;
    __le32 checksum;
};
