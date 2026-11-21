/* LinuxKPI: the page model.
 *
 * alloc_pages(order) takes a block of 2^order physical pages from the
 * kernel's buddy allocator (reached through the direct map) and gives it
 * an array of 2^order struct page descriptors, one per 4 KiB page, so page
 * arithmetic (page + i, nth_page) works as in Linux. The first is the head:
 * it holds the order and the reference count; every page points to its head.
 * A pfn table maps pfns back to descriptors (pfn_to_page, virt_to_page);
 * RAM pages that never came from alloc_pages() (kmalloc memory in an sg
 * list) get a permanent descriptor on first lookup. A folio is a head page. */
#ifndef _LKPI_LINUX_PAGE_H
#define _LKPI_LINUX_PAGE_H

#include <linux/lkpi_all.h>
#include <linux/mm_types.h>

LKPI_EXTERN_C_BEGIN

struct address_space;
struct page {
	void *virt;                     /* direct-map address */
	unsigned int order;
	atomic_t refcount;
	u32 magic;
	struct address_space *mapping;  /* shmem file it belongs to, if any */
	unsigned long index;            /* page index within that file */
	unsigned long private;
	unsigned long flags;            /* PG_dirty, PG_referenced */
	struct list_head lru;           /* free for the owner's lists (TTM pools, ...) */
	struct page *head;              /* first page of the block (itself for a head page) */
	unsigned long dma_addr;         /* page_pool / driver use */
};
struct folio { struct page page; };
#define PG_dirty 0x1UL
#define PG_referenced 0x2UL

struct page *alloc_pages(gfp_t gfp, unsigned int order);
#define alloc_page(gfp) alloc_pages(gfp, 0)
#define alloc_pages_node(nid, gfp, order) alloc_pages(gfp, order)
#define dev_alloc_pages(order) alloc_pages(GFP_ATOMIC, order)
#define dev_alloc_page() alloc_pages(GFP_ATOMIC, 0)
void __free_pages(struct page *page, unsigned int order);
#define __free_page(p) __free_pages(p, 0)
void get_page(struct page *page);
void put_page(struct page *page);
void *lkpi_page_address(const struct page *page);
struct page *lkpi_pfn_to_page(unsigned long pfn);
#define pfn_to_page(pfn) lkpi_pfn_to_page(pfn)
/* Across separately allocated blocks too (i915 merges contiguous pfns into one sg entry). */
#define nth_page(p, n) lkpi_pfn_to_page(page_to_pfn(p) + (n))
#define compound_head(p) ((p)->head)
#define pfn_valid(pfn) true
void split_page(struct page *page, unsigned int order);
#define page_address(p) lkpi_page_address(p)
#define page_to_virt(p) lkpi_page_address(p)
struct page *lkpi_virt_to_page(const void *addr);
#define virt_to_page(a) lkpi_virt_to_page(a)
#define virt_to_head_page(a) lkpi_virt_to_page(a)
unsigned long __get_free_pages(gfp_t gfp, unsigned int order);
#define __get_free_page(gfp) __get_free_pages(gfp, 0)
#define get_zeroed_page(gfp) __get_free_pages((gfp) | __GFP_ZERO, 0)
void free_pages(unsigned long addr, unsigned int order);
#define free_page(addr) free_pages(addr, 0)

u64 lkpi_direct_to_phys(const void *virt);
void *lkpi_phys_to_direct(u64 phys);
#define page_to_phys(p) lkpi_direct_to_phys(lkpi_page_address(p))
#define page_to_pfn(p) ((unsigned long)(page_to_phys(p) >> PAGE_SHIFT))
#define virt_to_phys(v) lkpi_direct_to_phys(v)
#define phys_to_virt(p) lkpi_phys_to_direct(p)
#define __pa(v) lkpi_direct_to_phys((const void *)(v))
#define __va(p) lkpi_phys_to_direct(p)
#define PHYS_PFN(x) ((unsigned long)((x) >> PAGE_SHIFT))
#define PFN_PHYS(x) ((u64)(x) << PAGE_SHIFT)
#define page_count(p) atomic_read(&(p)->head->refcount)
#define page_ref_count(p) page_count(p)
#define compound_order(p) ((p)->head == (p) ? (p)->order : 0)
#define page_size(p) (PAGE_SIZE << compound_order(p))
#define set_page_dirty(p) ((p)->flags |= PG_dirty, 1)
#define set_page_dirty_lock(p) set_page_dirty(p)
#define mark_page_accessed(p) ((p)->flags |= PG_referenced)
#define PageHighMem(p) false

/* Always mapped: kmap is the direct-map address. */
static inline void *kmap(struct page *p) { return lkpi_page_address(p); }
static inline void kunmap(struct page *p) { (void)p; }
#define kmap_atomic(p) lkpi_page_address(p)
#define kunmap_atomic(addr) do { (void)(addr); } while (0)
#define kmap_local_page(p) lkpi_page_address(p)
#define kmap_local_folio(f, off) ((char *)lkpi_page_address(&(f)->page) + (off))
#define kunmap_local(addr) do { (void)(addr); } while (0)

/* Folios. */
#define page_folio(p) ((struct folio *)((p)->head))
#define folio_page(f, n) (&(f)->page + (n))
#define folio_file_page(f, index) (&(f)->page + ((index) & ((1UL << (f)->page.order) - 1)))
#define folio_nr_pages(f) (1UL << (f)->page.order)
#define folio_size(f) (PAGE_SIZE << (f)->page.order)
#define folio_order(f) ((f)->page.order)
#define folio_pfn(f) page_to_pfn(&(f)->page)
#define folio_address(f) lkpi_page_address(&(f)->page)
#define folio_get(f) get_page(&(f)->page)
#define folio_put(f) put_page(&(f)->page)
#define folio_mark_dirty(f) ((f)->page.flags |= PG_dirty, true)
#define folio_mark_accessed(f) ((f)->page.flags |= PG_referenced)
#define folio_test_dirty(f) (((f)->page.flags & PG_dirty) != 0)
#define folio_lock(f) do { } while (0)
#define folio_unlock(f) do { } while (0)
#define folio_test_highmem(f) false

#define PAGEVEC_SIZE 31
struct folio_batch {
	unsigned char nr;
	unsigned char i;
	bool percpu_pvec_drained;
	struct folio *folios[PAGEVEC_SIZE];
};
static inline void folio_batch_init(struct folio_batch *fb) { fb->nr = 0; fb->i = 0; fb->percpu_pvec_drained = false; }
static inline unsigned int folio_batch_count(struct folio_batch *fb) { return fb->nr; }
static inline unsigned int folio_batch_space(struct folio_batch *fb) { return PAGEVEC_SIZE - fb->nr; }
static inline unsigned folio_batch_add(struct folio_batch *fb, struct folio *f)
{
	fb->folios[fb->nr++] = f;
	return folio_batch_space(fb);
}
void __folio_batch_release(struct folio_batch *fb);
#define folio_batch_release(fb) __folio_batch_release(fb)
#define check_move_unevictable_folios(fb) do { } while (0)

/* Kernel mappings of page arrays. */
#define VM_MAP 0x00000004
#define VM_IOREMAP 0x00000001
void *vmap(struct page **pages, unsigned int count, unsigned long flags, pgprot_t prot);
void vunmap(const void *addr);
void *vmap_pfn(unsigned long *pfns, unsigned int count, pgprot_t prot);
/* Cache attributes of the direct map: everything stays write-back here;
 * drivers flush with clflush where they need coherency. */
#define set_pages_array_wc(pages, n) 0
#define set_pages_array_uc(pages, n) 0
#define set_pages_array_wb(pages, n) 0
#define set_pages_wb(page, n) 0
#define set_memory_wc(addr, n) 0
#define set_memory_wb(addr, n) 0

/* Shared memory files (GEM objects' backing store). */
struct file;
struct file *shmem_file_setup(const char *name, loff_t size, unsigned long flags);
struct folio *shmem_read_folio_gfp(struct address_space *mapping, pgoff_t index, gfp_t gfp);
struct page *shmem_read_mapping_page_gfp(struct address_space *mapping, pgoff_t index, gfp_t gfp);
#define shmem_read_mapping_page(m, i) shmem_read_mapping_page_gfp(m, i, GFP_KERNEL)
void shmem_truncate_range(struct inode *inode, loff_t start, loff_t end);
#define mapping_gfp_mask(m) ((gfp_t)GFP_KERNEL)
#define mapping_gfp_constraint(m, g) ((gfp_t)(GFP_KERNEL & (g)))
#define mapping_set_gfp_mask(m, g) do { (void)(m); (void)(g); } while (0)
#define mapping_set_unevictable(m) do { (void)(m); } while (0)
#define mapping_clear_unevictable(m) do { (void)(m); } while (0)
#define invalidate_mapping_pages(m, start, end) 0UL
#define is_cow_mapping(flags) (((flags) & (VM_SHARED | VM_MAYWRITE)) == VM_MAYWRITE)

LKPI_EXTERN_C_END
#endif
