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
struct page *alloc_pages(gfp_t gfp, unsigned int order)
{
	struct page *p = kzalloc(sizeof(struct folio), gfp);
	if (!p)
		return NULL;
	p->virt = lkpi_phys_alloc(order, (gfp & __GFP_ZERO) != 0);
	if (!p->virt) {
		kfree(p);
		return NULL;
	}
	p->order = order;
	p->magic = LKPI_PAGE_MAGIC;
	atomic_set(&p->refcount, 1);
	return p;
}

void __free_pages(struct page *page, unsigned int order) { put_page(page); }
void get_page(struct page *page) { atomic_inc(&page->refcount); }

void put_page(struct page *page)
{
	if (!page || !atomic_dec_and_test(&page->refcount))
		return;
	if (page->magic != LKPI_PAGE_MAGIC) {
		printk(KERN_ERR "linuxkpi: put_page of a bad page %p\n", page);
		return;
	}
	page->magic = 0;
	lkpi_phys_free(page->virt, page->order);
	kfree(page);
}

void *lkpi_page_address(const struct page *page) { return page->virt; }

struct page *lkpi_virt_to_page(const void *addr)
{
	/* Descriptors live beside the memory, not in a page database. */
	printk(KERN_ERR "linuxkpi: virt_to_page() is not supported\n");
	return NULL;
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
	unsigned int n = 0, i, j;
	u64 *phys;
	void *v;
	for (i = 0; i < count; i++)
		n += 1u << pages[i]->order;
	phys = kmalloc_array(n, sizeof(u64), GFP_KERNEL);
	if (!phys)
		return NULL;
	n = 0;
	for (i = 0; i < count; i++)
		for (j = 0; j < (1u << pages[i]->order); j++)
			phys[n++] = page_to_phys(pages[i]) + (u64)j * PAGE_SIZE;
	v = vmap_phys_list(phys, n, prot);
	kfree(phys);
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
