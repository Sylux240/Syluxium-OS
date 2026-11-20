/* LinuxKPI: the page model (see <linux/lkpi_page.h>), kernel mappings of
 * page arrays, scatter/gather tables of pages and shmem files, which
 * back DRM's GEM buffer objects. */
#include <linux/lkpi_all.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>

#define LKPI_PAGE_MAGIC 0x50414745u

void *lkpi_phys_alloc(unsigned order, bool zero);
void lkpi_phys_free(void *virt, unsigned order);
void *lkpi_vmap_phys(const u64 *phys, unsigned long count, bool wc);
void lkpi_vunmap(const void *virt);

/* ---- pages ---- */

/* pfn -> descriptor: a directory of 1 GiB chunks (2 MiB tables of
 * 2^18 pointers, allocated on first use), covering 4 TiB of physical
 * addresses. */
#define PFN_CHUNK_SHIFT 18
#define PFN_DIR_SIZE 4096
#define PFN_TABLE_ORDER (PFN_CHUNK_SHIFT + 3 - PAGE_SHIFT)
static struct page **pfn_dir[PFN_DIR_SIZE];
static DEFINE_SPINLOCK(pfn_dir_lock);

static struct page **pfn_slot(unsigned long pfn, bool create)
{
	unsigned long d = pfn >> PFN_CHUNK_SHIFT;
	struct page **t;
	if (d >= PFN_DIR_SIZE)
		return NULL;
	t = __atomic_load_n(&pfn_dir[d], __ATOMIC_ACQUIRE);
	if (!t && create) {
		struct page **n = lkpi_phys_alloc(PFN_TABLE_ORDER, true);
		spin_lock(&pfn_dir_lock);
		t = pfn_dir[d];
		if (!t && n) {
			__atomic_store_n(&pfn_dir[d], n, __ATOMIC_RELEASE);
			t = n;
			n = NULL;
		}
		spin_unlock(&pfn_dir_lock);
		if (n)
			lkpi_phys_free(n, PFN_TABLE_ORDER);
	}
	return t ? &t[pfn & ((1UL << PFN_CHUNK_SHIFT) - 1)] : NULL;
}

#define LKPI_PAGE_FOREIGN 0x464f524eu /* a permanent descriptor for memory we didn't allocate */

struct page *lkpi_pfn_to_page(unsigned long pfn)
{
	struct page **slot = pfn_slot(pfn, true), *p, *expected = NULL;
	if (!slot)
		return NULL;
	p = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
	if (p)
		return p;
	p = kzalloc(sizeof(*p), GFP_ATOMIC);
	if (!p)
		return NULL;
	p->virt = lkpi_phys_to_direct(PFN_PHYS(pfn));
	p->head = p;
	p->magic = LKPI_PAGE_FOREIGN;
	atomic_set(&p->refcount, 1 << 30);
	INIT_LIST_HEAD(&p->lru);
	if (!__atomic_compare_exchange_n(slot, &expected, p, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		kfree(p);
		return expected;
	}
	return p;
}

struct page *alloc_pages(gfp_t gfp, unsigned int order)
{
	unsigned long n = 1UL << order, pfn;
	struct page *p = kvzalloc(n * sizeof(struct page), gfp & ~__GFP_ZERO);
	void *virt;
	if (!p)
		return NULL;
	virt = lkpi_phys_alloc(order, (gfp & __GFP_ZERO) != 0);
	if (!virt) {
		kvfree(p);
		return NULL;
	}
	pfn = lkpi_direct_to_phys(virt) >> PAGE_SHIFT;
	for (unsigned long i = 0; i < n; i++) {
		struct page **slot = pfn_slot(pfn + i, true);
		p[i].virt = (char *)virt + i * PAGE_SIZE;
		p[i].head = p;
		p[i].magic = LKPI_PAGE_MAGIC;
		INIT_LIST_HEAD(&p[i].lru);
		if (slot)
			__atomic_store_n(slot, &p[i], __ATOMIC_RELEASE);
	}
	p->order = order;
	atomic_set(&p->refcount, 1);
	return p;
}

void __free_pages(struct page *page, unsigned int order) { put_page(page); }
void get_page(struct page *page) { atomic_inc(&page->head->refcount); }

void put_page(struct page *page)
{
	struct page *head;
	unsigned long n, pfn;
	if (!page)
		return;
	head = page->head;
	if (head->magic == LKPI_PAGE_FOREIGN)
		return;
	if (!atomic_dec_and_test(&head->refcount))
		return;
	if (head->magic != LKPI_PAGE_MAGIC) {
		printk(KERN_ERR "linuxkpi: put_page of a bad page %p\n", page);
		return;
	}
	n = 1UL << head->order;
	pfn = lkpi_direct_to_phys(head->virt) >> PAGE_SHIFT;
	for (unsigned long i = 0; i < n; i++) {
		struct page **slot = pfn_slot(pfn + i, false);
		if (slot)
			__atomic_store_n(slot, NULL, __ATOMIC_RELEASE);
		head[i].magic = 0;
	}
	lkpi_phys_free(head->virt, head->order);
	kvfree(head);
}

/* Linux splits a non-compound block into independent pages; here the block
 * keeps one descriptor array, so it is freed once every page was put. */
void split_page(struct page *page, unsigned int order)
{
	atomic_add((1 << order) - 1, &page->head->refcount);
}

void *lkpi_page_address(const struct page *page) { return page->virt; }

struct page *lkpi_virt_to_page(const void *addr)
{
	return lkpi_pfn_to_page(lkpi_direct_to_phys(addr) >> PAGE_SHIFT);
}

unsigned long __get_free_pages(gfp_t gfp, unsigned int order)
{
	return (unsigned long)lkpi_phys_alloc(order, (gfp & __GFP_ZERO) != 0);
}

void free_pages(unsigned long addr, unsigned int order)
{
	if (addr)
		lkpi_phys_free((void *)addr, order);
}

void __folio_batch_release(struct folio_batch *fb)
{
	for (unsigned i = 0; i < fb->nr; i++)
		folio_put(fb->folios[i]);
	fb->nr = 0;
}

/* ---- kernel mappings ---- */
static void *vmap_phys_list(u64 *phys, unsigned int count, pgprot_t prot)
{
	return lkpi_vmap_phys(phys, count, (pgprot_val(prot) & (LKPI_PROT_WC | LKPI_PROT_UC)) != 0);
}

void *vmap(struct page **pages, unsigned int count, unsigned long flags, pgprot_t prot)
{
	u64 *phys;
	void *v;
	phys = kvmalloc_array(count, sizeof(u64), GFP_KERNEL);
	if (!phys)
		return NULL;
	for (unsigned int i = 0; i < count; i++)
		phys[i] = page_to_phys(pages[i]);
	v = vmap_phys_list(phys, count, prot);
	kvfree(phys);
	return v;
}

void *vmap_pfn(unsigned long *pfns, unsigned int count, pgprot_t prot)
{
	u64 *phys = kmalloc_array(count, sizeof(u64), GFP_KERNEL);
	void *v;
	if (!phys)
		return NULL;
	for (unsigned i = 0; i < count; i++)
		phys[i] = PFN_PHYS(pfns[i]);
	v = vmap_phys_list(phys, count, prot);
	kfree(phys);
	return v;
}

void vunmap(const void *addr)
{
	if (addr)
		lkpi_vunmap(addr);
}

/* ---- scatter/gather tables of pages (no IOMMU: DMA address = physical) ---- */
int sg_alloc_table_from_pages_segment(struct sg_table *sgt, struct page **pages, unsigned int n_pages,
				      unsigned int offset, unsigned long size, unsigned int max_segment, gfp_t gfp)
{
	struct scatterlist *sg;
	unsigned int i;
	int r = sg_alloc_table(sgt, n_pages, gfp);
	if (r)
		return r;
	for_each_sg(sgt->sgl, sg, n_pages, i) {
		unsigned int len = PAGE_SIZE - (i == 0 ? offset : 0);
		if (len > size)
			len = size;
		sg_set_page(sg, pages[i], len, i == 0 ? offset : 0);
		size -= len;
	}
	return 0;
}

int dma_map_sgtable(struct device *dev, struct sg_table *sgt, enum dma_data_direction dir, unsigned long attrs)
{
	struct scatterlist *sg;
	unsigned int i;
	for_each_sg(sgt->sgl, sg, sgt->orig_nents, i) {
		sg->dma_address = lkpi_direct_to_phys(sg_virt(sg));
		sg->dma_length = sg->length;
	}
	sgt->nents = sgt->orig_nents;
	return 0;
}

void dma_unmap_sgtable(struct device *dev, struct sg_table *sgt, enum dma_data_direction dir, unsigned long attrs) { }

/* ---- shmem files: a growable array of pages, filled on first use ---- */
struct lkpi_shmem {
	struct mutex lock;
	unsigned long npages;
	struct page **pages;
};

static int shmem_release(struct inode *inode, struct file *f)
{
	struct lkpi_shmem *sh = inode->i_data.lkpi;
	if (sh) {
		for (unsigned long i = 0; i < sh->npages; i++)
			if (sh->pages[i])
				put_page(sh->pages[i]);
		kvfree(sh->pages);
		kfree(sh);
	}
	kfree(inode);
	return 0;
}

static const struct file_operations shmem_fops = { .release = shmem_release };

struct file *shmem_file_setup(const char *name, loff_t size, unsigned long flags)
{
	struct lkpi_shmem *sh = kzalloc(sizeof(*sh), GFP_KERNEL);
	struct file *f;
	if (!sh)
		return ERR_PTR(-ENOMEM);
	mutex_init(&sh->lock);
	sh->npages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
	sh->pages = kvcalloc(sh->npages ? sh->npages : 1, sizeof(struct page *), GFP_KERNEL);
	f = sh->pages ? lkpi_file_alloc(&shmem_fops, NULL, O_RDWR, NULL) : NULL;
	if (!f) {
		kvfree(sh->pages);
		kfree(sh);
		return ERR_PTR(-ENOMEM);
	}
	f->f_inode->i_size = size;
	f->f_inode->i_data.lkpi = sh;
	return f;
}

struct page *shmem_read_mapping_page_gfp(struct address_space *mapping, pgoff_t index, gfp_t gfp)
{
	struct lkpi_shmem *sh = mapping->lkpi;
	struct page *p;
	if (!sh || index >= sh->npages)
		return ERR_PTR(-EINVAL);
	mutex_lock(&sh->lock);
	p = sh->pages[index];
	if (!p) {
		p = alloc_page(gfp | __GFP_ZERO);
		if (p) {
			p->mapping = mapping;
			p->index = index;
			sh->pages[index] = p;
		}
	}
	if (p)
		get_page(p); /* the caller's reference */
	mutex_unlock(&sh->lock);
	return p ? p : ERR_PTR(-ENOMEM);
}

struct folio *shmem_read_folio_gfp(struct address_space *mapping, pgoff_t index, gfp_t gfp)
{
	struct page *p = shmem_read_mapping_page_gfp(mapping, index, gfp);
	return IS_ERR(p) ? (struct folio *)p : page_folio(p);
}

void shmem_truncate_range(struct inode *inode, loff_t start, loff_t end)
{
	struct lkpi_shmem *sh = inode->i_data.lkpi;
	unsigned long first = (start + PAGE_SIZE - 1) >> PAGE_SHIFT;
	unsigned long last = end < 0 ? sh->npages : ((unsigned long)end + 1) >> PAGE_SHIFT;
	if (!sh)
		return;
	mutex_lock(&sh->lock);
	for (unsigned long i = first; i < last && i < sh->npages; i++) {
		if (sh->pages[i]) {
			put_page(sh->pages[i]);
			sh->pages[i] = NULL;
		}
	}
	mutex_unlock(&sh->lock);
}
