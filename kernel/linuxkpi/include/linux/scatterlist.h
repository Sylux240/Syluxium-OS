/* LinuxKPI: scatter/gather lists of kernel-virtual buffers (no chaining). */
#ifndef _LKPI_LINUX_SCATTERLIST_H
#define _LKPI_LINUX_SCATTERLIST_H

#include <linux/kernel.h>

LKPI_EXTERN_C_BEGIN

struct scatterlist {
    unsigned long page_link; /* bit 1: last entry */
    unsigned int offset;
    unsigned int length;
    dma_addr_t dma_address;
    unsigned int dma_length;
    void* lkpi_buf;
};
#define SG_END 0x02UL

static inline void sg_mark_end(struct scatterlist* sg) { sg->page_link |= SG_END; }
static inline void sg_unmark_end(struct scatterlist* sg) { sg->page_link &= ~SG_END; }
static inline bool sg_is_last(struct scatterlist* sg) { return sg->page_link & SG_END; }
static inline void sg_init_table(struct scatterlist* sgl, unsigned int nents) {
    memset(sgl, 0, sizeof(*sgl) * nents);
    sg_mark_end(&sgl[nents - 1]);
}
static inline void sg_set_buf(struct scatterlist* sg, const void* buf, unsigned int buflen) {
    sg->lkpi_buf = (void*)buf;
    sg->offset = 0;
    sg->length = buflen;
}
static inline void sg_init_one(struct scatterlist* sg, const void* buf, unsigned int buflen) {
    sg_init_table(sg, 1);
    sg_set_buf(sg, buf, buflen);
}
static inline void* sg_virt(struct scatterlist* sg) { return (char*)sg->lkpi_buf + sg->offset; }
static inline struct scatterlist* sg_next(struct scatterlist* sg) { return sg_is_last(sg) ? NULL : sg + 1; }
#define for_each_sg(sglist, sg, nr, __i) for (__i = 0, sg = (sglist); __i < (nr); __i++, sg = sg_next(sg))
#define sg_dma_address(sg) ((sg)->dma_address)
#define sg_dma_len(sg) ((sg)->dma_length)

LKPI_EXTERN_C_END

#endif
