/* USB Wi-Fi drivers with a scatter-gather transfer path (mt76's usb.c):
 * the host controllers here announce no SG support (usb_bus.sg_tablesize
 * 0), so the drivers use linear buffers; the SG helpers only have to
 * compile. Force-included after net/lkpi_netcompat.h (Makefile). */
#pragma once
#include <net/lkpi_netcompat.h>
#include <linux/scatterlist.h>

static inline void sg_init_marker(struct scatterlist *sgl, unsigned int nents) { sg_mark_end(&sgl[nents - 1]); }
static inline int skb_to_sgvec(struct sk_buff *skb, struct scatterlist *sg, int offset, int len) { return -ENOSYS; }
