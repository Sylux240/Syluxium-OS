/* LinuxKPI: GPU driver support - display aperture handover (Linux's
 * drivers/video/aperture.c) and the boot-time display driver choice.
 *
 * The firmware framebuffer is shown by simpledrm on a "simple-framebuffer"
 * platform device (registered by linuxkpi::init, before PCI drivers probe).
 * A real GPU driver (i915, amdgpu, radeon, nouveau, bochs) claims the PCI
 * memory ranges of its device with aperture_remove_conflicting_pci_devices()
 * early in probe; if the boot framebuffer lives in one of them, simpledrm is
 * removed (the GPU driver then gets /dev/dri/card0) and the kernel stops
 * drawing on the old framebuffer. If the GPU driver's probe then fails with
 * no DRM device left, the firmware framebuffer goes back to simpledrm
 * (lkpi_gpu_probe_failed), so the desktop still comes up. */
#include <linux/lkpi_all.h>
#include <linux/aperture.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

static struct {
	resource_size_t base, size;     /* the firmware framebuffer */
	u32 width, height, pitch, red_shift;
	bool known;
	bool taken;                     /* a GPU driver claimed it */
	const char *taken_by;
	struct platform_device *pdev;   /* simple-framebuffer device, while registered */
} boot_fb;
static DEFINE_MUTEX(aperture_lock);
static int drm_cards; /* /dev/dri/cardN nodes */

void lkpi_register_boot_framebuffer(u64 phys, u32 width, u32 height, u32 pitch, u32 red_shift);
void lkpi_display_released(void); /* core.cpp: the kernel stops drawing on the firmware framebuffer */
void lkpi_display_restored(void);  /* core.cpp: ... and draws there again */

/* Called by core.cpp before any driver probes. */
void lkpi_boot_fb_set(u64 base, u32 width, u32 height, u32 pitch, u32 red_shift)
{
	boot_fb.base = base;
	boot_fb.size = (u64)pitch * height;
	boot_fb.width = width;
	boot_fb.height = height;
	boot_fb.pitch = pitch;
	boot_fb.red_shift = red_shift;
	boot_fb.known = boot_fb.size != 0;
}

void lkpi_boot_fb_registered(struct platform_device *pdev)
{
	boot_fb.pdev = pdev;
}

/* device_add()/device_del() of a /dev/dri/cardN node. */
void lkpi_drm_card_added(int delta)
{
	__atomic_add_fetch(&drm_cards, delta, __ATOMIC_RELAXED);
}

/* pci.cpp: a display controller's driver failed to probe. If it had taken
 * the display over and left no DRM device behind, give the firmware
 * framebuffer back to simpledrm so the desktop still comes up. */
void lkpi_gpu_probe_failed(const char *driver)
{
	bool restore;
	mutex_lock(&aperture_lock);
	restore = boot_fb.taken && !boot_fb.pdev && __atomic_load_n(&drm_cards, __ATOMIC_RELAXED) == 0;
	if (restore)
		boot_fb.taken = false;
	mutex_unlock(&aperture_lock);
	if (!restore)
		return;
	printk(KERN_WARNING "aperture: %s failed after taking over the display: back to the firmware framebuffer\n",
	       driver ? driver : "the GPU driver");
	lkpi_register_boot_framebuffer(boot_fb.base, boot_fb.width, boot_fb.height, boot_fb.pitch, boot_fb.red_shift);
	lkpi_display_restored();
}

int devm_aperture_acquire_for_platform_device(struct platform_device *pdev, resource_size_t base,
					      resource_size_t size)
{
	return 0; /* the only firmware device is the boot framebuffer, tracked above */
}

static void remove_boot_fb(const char *name)
{
	struct platform_device *pdev;
	bool first;
	mutex_lock(&aperture_lock);
	pdev = boot_fb.pdev;
	boot_fb.pdev = NULL;
	first = !boot_fb.taken;
	boot_fb.taken = true;
	boot_fb.taken_by = name;
	mutex_unlock(&aperture_lock);
	if (first) {
		printk(KERN_INFO "aperture: %s takes over the display from the firmware framebuffer\n", name);
		lkpi_display_released();
	}
	if (pdev)
		platform_device_unregister(pdev); /* simpledrm's remove: its /dev/dri/card goes away */
}

int aperture_remove_conflicting_devices(resource_size_t base, resource_size_t size, const char *name)
{
	if (boot_fb.known && base < boot_fb.base + boot_fb.size && boot_fb.base < base + size)
		remove_boot_fb(name);
	return 0;
}

int __aperture_remove_legacy_vga_devices(struct pci_dev *pdev)
{
	return 0; /* no vgacon / vga16fb */
}

int aperture_remove_conflicting_pci_devices(struct pci_dev *pdev, const char *name)
{
	bool primary = false;
	for (int bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		if (!(pci_resource_flags(pdev, bar) & IORESOURCE_MEM) || !pci_resource_len(pdev, bar))
			continue;
		if (boot_fb.known && pci_resource_start(pdev, bar) < boot_fb.base + boot_fb.size &&
		    boot_fb.base < pci_resource_start(pdev, bar) + pci_resource_len(pdev, bar))
			primary = true;
	}
	/* Linux also kicks the firmware framebuffer out for the primary VGA
	 * device; with one display controller that is this one. On Intel the
	 * GOP framebuffer can sit in stolen memory outside every BAR. */
	if (!primary && boot_fb.known && (pdev->class >> 8) == PCI_CLASS_DISPLAY_VGA &&
	    pdev->vendor == PCI_VENDOR_ID_INTEL)
		primary = true;
	if (primary)
		remove_boot_fb(name);
	return 0;
}

/* ---- DMA (no IOMMU: bus address = physical address) ---- */
dma_addr_t dma_map_page(struct device *dev, struct page *page, size_t offset, size_t size, enum dma_data_direction dir)
{
	return page_to_phys(page) + offset;
}

struct resource lkpi_dummy_resource = { .name = "reserved" };
struct resource ioport_resource = { .name = "PCI IO", .start = 0, .end = 0xffff, .flags = IORESOURCE_IO };

/* ---- memory queries & shrinkers ---- */
u64 lkpi_kernel_virt_to_phys(const void *virt);
unsigned long lkpi_freeram_pages(void);

struct page *vmalloc_to_page(const void *addr)
{
	u64 phys = lkpi_kernel_virt_to_phys(addr);
	return phys ? pfn_to_page(phys >> PAGE_SHIFT) : NULL;
}

unsigned long vmalloc_to_pfn(const void *addr)
{
	return lkpi_kernel_virt_to_phys(addr) >> PAGE_SHIFT;
}

void si_meminfo(struct sysinfo *val)
{
	memset(val, 0, sizeof(*val));
	val->totalram = lkpi_totalram_pages();
	val->freeram = lkpi_freeram_pages();
	val->mem_unit = PAGE_SIZE;
}

struct shrinker *shrinker_alloc(unsigned int flags, const char *fmt, ...)
{
	return kzalloc(sizeof(struct shrinker), GFP_KERNEL);
}

void shrinker_free(struct shrinker *s)
{
	kfree(s);
}

/* ---- assorted kernel library functions GPU drivers use ---- */
#include <linux/bitmap.h>
#include <linux/sort.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>
#include <linux/scatterlist.h>
#include <linux/wait_bit.h>

/* Waiting on bits / variables: a small hashed table of wait queues. */
#define BIT_WAIT_TABLE 64
static wait_queue_head_t bit_wait_table[BIT_WAIT_TABLE];
static int bit_wait_ready;
wait_queue_head_t *bit_waitqueue(void *word, int bit)
{
	unsigned long h = ((unsigned long)word >> 3) ^ (unsigned long)bit;
	if (!__atomic_load_n(&bit_wait_ready, __ATOMIC_ACQUIRE)) {
		static DEFINE_SPINLOCK(init_lock);
		spin_lock(&init_lock);
		if (!bit_wait_ready) {
			for (int i = 0; i < BIT_WAIT_TABLE; i++)
				init_waitqueue_head(&bit_wait_table[i]);
			__atomic_store_n(&bit_wait_ready, 1, __ATOMIC_RELEASE);
		}
		spin_unlock(&init_lock);
	}
	h ^= h >> 7;
	return &bit_wait_table[h % BIT_WAIT_TABLE];
}

void wake_up_bit(void *word, int bit)
{
	wake_up_all(bit_waitqueue(word, bit));
}

void lkpi_rcu_work_fn(struct rcu_head *h)
{
	struct rcu_work *rw = container_of(h, struct rcu_work, rcu);
	queue_work(rw->wq, &rw->work);
}

int bitmap_find_free_region(unsigned long *bitmap, unsigned int bits, int order)
{
	unsigned int n = 1U << order;
	for (unsigned int pos = 0; pos + n <= bits; pos += n) {
		unsigned int i;
		for (i = 0; i < n && !test_bit(pos + i, bitmap); i++)
			;
		if (i == n) {
			for (i = 0; i < n; i++)
				__set_bit(pos + i, bitmap);
			return pos;
		}
	}
	return -ENOMEM;
}

void bitmap_release_region(unsigned long *bitmap, unsigned int pos, int order)
{
	for (unsigned int i = 0; i < (1U << order); i++)
		__clear_bit(pos + i, bitmap);
}

void bitmap_shift_right(unsigned long *dst, const unsigned long *src, unsigned int shift, unsigned int nbits)
{
	for (unsigned int i = 0; i < nbits; i++) {
		if (i + shift < nbits && test_bit(i + shift, src))
			__set_bit(i, dst);
		else
			__clear_bit(i, dst);
	}
}

/* sort with a context argument: insertion sort (small arrays in drivers). */
void lkpi_sort_r(void *base, size_t num, size_t size, int (*cmp)(const void *, const void *, const void *),
		 const void *priv)
{
	char *b = base, tmp[256];
	if (size > sizeof(tmp))
		return;
	for (size_t i = 1; i < num; i++) {
		size_t j = i;
		memcpy(tmp, b + i * size, size);
		while (j > 0 && cmp(b + (j - 1) * size, tmp, priv) > 0) {
			memcpy(b + j * size, b + (j - 1) * size, size);
			j--;
		}
		memcpy(b + j * size, tmp, size);
	}
}

void *bsearch(const void *key, const void *base, size_t num, size_t size, cmp_func_t cmp)
{
	size_t lo = 0, hi = num;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		const char *p = (const char *)base + mid * size;
		int r = cmp(key, p);
		if (r == 0)
			return (void *)p;
		if (r < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return NULL;
}

int lkpi_xa_alloc_cyclic(struct xarray *xa, u32 *id, void *entry, u32 min, u32 max, u32 *next, gfp_t gfp)
{
	u32 start = *next < min || *next > max ? min : *next;
	for (u32 i = start;; ) {
		if (!xa_load(xa, i)) {
			xa_store(xa, i, entry, gfp);
			*id = i;
			*next = i == max ? min : i + 1;
			return i < start ? 1 : 0; /* 1: wrapped */
		}
		i = i == max ? min : i + 1;
		if (i == start)
			return -EBUSY;
	}
}

const struct pci_device_id *lkpi_pci_match_id(const struct pci_device_id *ids, struct pci_dev *dev)
{
	for (; ids && (ids->vendor || ids->subvendor || ids->class_mask); ids++) {
		if ((ids->vendor == PCI_ANY_ID || ids->vendor == dev->vendor) &&
		    (ids->device == PCI_ANY_ID || ids->device == dev->device) &&
		    (ids->subvendor == PCI_ANY_ID || ids->subvendor == dev->subsystem_vendor) &&
		    (ids->subdevice == PCI_ANY_ID || ids->subdevice == dev->subsystem_device) &&
		    !((ids->class ^ dev->class) & ids->class_mask))
			return ids;
	}
	return NULL;
}

int lkpi_dma_map_sg(struct device *dev, struct scatterlist *sgl, int nents, int dir)
{
	struct scatterlist *sg;
	int i;
	for_each_sg(sgl, sg, nents, i) {
		sg->dma_address = lkpi_direct_to_phys(sg_virt(sg));
		sg->dma_length = sg->length;
	}
	return nents;
}

void *memdup_user_nul(const void __user *src, size_t len)
{
	char *p = kmalloc(len + 1, GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);
	if (copy_from_user(p, src, len)) {
		kfree(p);
		return ERR_PTR(-EFAULT);
	}
	p[len] = 0;
	return p;
}

int lkpi_kstrtobool_from_user(const char __user *s, size_t count, bool *res)
{
	char buf[8] = { 0 };
	if (copy_from_user(buf, s, min(count, sizeof(buf) - 1)))
		return -EFAULT;
	if (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y' || (buf[0] == 'o' && buf[1] == 'n')) {
		*res = true;
		return 0;
	}
	if (buf[0] == '0' || buf[0] == 'n' || buf[0] == 'N' || (buf[0] == 'o' && buf[1] == 'f')) {
		*res = false;
		return 0;
	}
	return -EINVAL;
}
