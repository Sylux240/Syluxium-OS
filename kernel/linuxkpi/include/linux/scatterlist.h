/* LinuxKPI: scatter/gather lists of kernel-virtual buffers (no chaining). */
#ifndef _LKPI_LINUX_SCATTERLIST_H
#define _LKPI_LINUX_SCATTERLIST_H

#include <linux/kernel.h>
#include <linux/lkpi_page.h>

LKPI_EXTERN_C_BEGIN

struct scatterlist {
    unsigned long page_link; /* bit 1: last entry */
    unsigned int offset;
    unsigned int length;
    dma_addr_t dma_address;
    unsigned int dma_length;
    void* lkpi_buf;
    struct page* lkpi_page; /* appended: set by sg_set_page() */
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
static inline void sg_set_page(struct scatterlist* sg, struct page* page, unsigned int len, unsigned int offset) {
    sg->lkpi_page = page;
    sg->lkpi_buf = lkpi_page_address(page);
    sg->offset = offset;
    sg->length = len;
}
static inline struct page* sg_page(struct scatterlist* sg) { return sg->lkpi_page; }
#define sg_phys(sg) (lkpi_direct_to_phys(sg_virt(sg)))

/* Tables (struct sg_table is declared with the other DRM needs). */
#define for_each_sgtable_sg(sgt, sg, i) for_each_sg((sgt)->sgl, sg, (sgt)->orig_nents, i)
#define for_each_sgtable_dma_sg(sgt, sg, i) for_each_sg((sgt)->sgl, sg, (sgt)->nents, i)
int sg_alloc_table_from_pages_segment(struct sg_table* sgt, struct page** pages, unsigned int n_pages,
                                      unsigned int offset, unsigned long size, unsigned int max_segment, gfp_t gfp);
#define sg_alloc_table_from_pages(sgt, pages, n, off, size, gfp) \
    sg_alloc_table_from_pages_segment(sgt, pages, n, off, size, UINT_MAX, gfp)

/* Page iterators: one step per PAGE_SIZE page of each entry. */
struct sg_page_iter { struct scatterlist* sg; unsigned int sg_pgoffset; unsigned int __nents; int __pg_advance; };
struct sg_dma_page_iter { struct sg_page_iter base; };
static inline unsigned int lkpi_sg_npages(struct scatterlist* sg) { return (sg->offset + sg->length + PAGE_SIZE - 1) >> PAGE_SHIFT; }
static inline bool lkpi_sg_page_iter_next(struct sg_page_iter* it, bool dma) {
    if (!it->sg || !it->__nents)
        return false;
    if (it->__pg_advance < 0) { /* first call */
        it->__pg_advance = 1;
        it->sg_pgoffset = 0;
    } else {
        it->sg_pgoffset++;
    }
    while (it->sg_pgoffset >= lkpi_sg_npages(it->sg)) {
        it->sg_pgoffset -= lkpi_sg_npages(it->sg);
        if (!--it->__nents || !(it->sg = sg_next(it->sg)))
            return false;
    }
    return true;
}
static inline void lkpi_sg_iter_start(struct sg_page_iter* it, struct scatterlist* sgl, unsigned int nents) {
    it->sg = sgl;
    it->__nents = nents;
    it->sg_pgoffset = 0;
    it->__pg_advance = -1;
}
#define for_each_sg_page(sglist, piter, nents, pgoffset) \
    for (lkpi_sg_iter_start(piter, sglist, nents); lkpi_sg_page_iter_next(piter, false);)
#define for_each_sgtable_page(sgt, piter, pgoffset) for_each_sg_page((sgt)->sgl, piter, (sgt)->orig_nents, pgoffset)
#define for_each_sgtable_dma_page(sgt, dpiter, pgoffset) \
    for (lkpi_sg_iter_start(&(dpiter)->base, (sgt)->sgl, (sgt)->nents); lkpi_sg_page_iter_next(&(dpiter)->base, true);)
/* Pages here are whole blocks, so page n of an entry is an address step. */
static inline u64 sg_page_iter_phys(struct sg_page_iter* it) {
    return lkpi_direct_to_phys(it->sg->lkpi_buf) + ((u64)it->sg_pgoffset << PAGE_SHIFT);
}
#define sg_page_iter_page(it) ((it)->sg->lkpi_page) /* only exact for single-page entries */
#define sg_page_iter_dma_address(dit) ((dit)->base.sg->dma_address + ((dma_addr_t)(dit)->base.sg_pgoffset << PAGE_SHIFT))
#define sg_dma_len(sg) ((sg)->dma_length)

LKPI_EXTERN_C_END

#endif
