/* LinuxKPI: Internet checksum helpers (net/checksum.h), in software. */
#pragma once
#include <linux/lkpi_all.h>

static inline __wsum csum_partial(const void *buff, int len, __wsum sum)
{
    const u8 *p = buff;
    u64 s = sum;
    while (len > 1) {
        s += (u32)p[0] | ((u32)p[1] << 8); /* little-endian 16-bit words, like x86 Linux */
        p += 2;
        len -= 2;
    }
    if (len)
        s += p[0];
    while (s >> 32)
        s = (s & 0xffffffff) + (s >> 32);
    return (__wsum)s;
}

static inline __sum16 csum_fold(__wsum csum)
{
    u32 sum = (u32)csum;
    sum = (sum & 0xffff) + (sum >> 16);
    sum = (sum & 0xffff) + (sum >> 16);
    return (__sum16)~sum;
}

static inline __wsum csum_add(__wsum csum, __wsum addend)
{
    u32 res = (u32)csum + (u32)addend;
    return (__wsum)(res + (res < (u32)addend));
}

static inline void csum_replace_by_diff(__sum16 *sum, __wsum diff)
{
    *sum = csum_fold(csum_add(diff, ~(__wsum)(u16)*sum));
}

/* TCP/UDP pseudo-header checksums: only for offloads drivers never get here. */
static inline __sum16 csum_tcpudp_magic(__be32 saddr, __be32 daddr, u32 len, u8 proto, __wsum sum) { return 0; }
