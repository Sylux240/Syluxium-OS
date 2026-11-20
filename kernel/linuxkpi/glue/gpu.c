/* LinuxKPI: GPU driver support - display aperture handover (Linux's
 * drivers/video/aperture.c) and the boot-time display driver choice.
 *
 * The firmware framebuffer is shown by simpledrm on a "simple-framebuffer"
 * platform device. A real GPU driver (i915, amdgpu, nouveau, virtio-gpu, ...)
 * claims the PCI memory ranges of its device with
 * aperture_remove_conflicting_pci_devices() early in probe; if the boot
 * framebuffer lives in one of them, it is removed (and, as GPU drivers bind
 * from module_init before the boot framebuffer is registered, normally never
 * registered at all). If the GPU driver then fails, linuxkpi::init() puts the
 * boot framebuffer back (lkpi_boot_fb_wanted()), so the desktop still comes
 * up on simpledrm. */
#include <linux/lkpi_all.h>
#include <linux/aperture.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

static struct {
	resource_size_t base, size;     /* the firmware framebuffer */
	bool known;
	bool taken;                     /* a GPU driver claimed it */
	const char *taken_by;
	struct platform_device *pdev;   /* simple-framebuffer device, while registered */
} boot_fb;
static DEFINE_MUTEX(aperture_lock);
static int drm_cards; /* /dev/dri/cardN nodes created */

/* Called by core.cpp before the drivers' module_init()s run. */
void lkpi_boot_fb_set_range(u64 base, u64 size)
{
	boot_fb.base = base;
	boot_fb.size = size;
	boot_fb.known = size != 0;
}

void lkpi_boot_fb_registered(struct platform_device *pdev)
{
	boot_fb.pdev = pdev;
}

/* After module_init: register the boot framebuffer unless a GPU driver
 * took the display over and now drives a /dev/dri/card. */
bool lkpi_boot_fb_wanted(void)
{
	if (!boot_fb.taken)
		return true;
	if (drm_cards > 0)
		return false;
	printk(KERN_WARNING "aperture: %s took over the display but registered no DRM device: "
	       "using the firmware framebuffer again\n", boot_fb.taken_by ? boot_fb.taken_by : "a GPU driver");
	boot_fb.taken = false;
	return true;
}

bool lkpi_boot_fb_taken(void)
{
	return boot_fb.taken;
}

/* device_add()/device_del() of a /dev/dri/cardN node. */
void lkpi_drm_card_added(int delta)
{
	__atomic_add_fetch(&drm_cards, delta, __ATOMIC_RELAXED);
}

int devm_aperture_acquire_for_platform_device(struct platform_device *pdev, resource_size_t base,
					      resource_size_t size)
{
	return 0; /* the only firmware device is the boot framebuffer, tracked above */
}

static void remove_boot_fb(const char *name)
{
	struct platform_device *pdev;
	mutex_lock(&aperture_lock);
	pdev = boot_fb.pdev;
	boot_fb.pdev = NULL;
	if (!boot_fb.taken)
		printk(KERN_INFO "aperture: %s takes over the display from the firmware framebuffer\n", name);
	boot_fb.taken = true;
	boot_fb.taken_by = name;
	mutex_unlock(&aperture_lock);
	if (pdev)
		platform_device_unregister(pdev); /* simpledrm's remove: /dev/dri/card0 goes away */
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
