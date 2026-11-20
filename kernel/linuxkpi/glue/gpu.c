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
