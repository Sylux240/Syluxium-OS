/* LinuxKPI: display apertures (Linux drivers/video/aperture.c).
 * The firmware framebuffer (simpledrm on the "simple-framebuffer"
 * device) owns the boot display until a GPU driver claims the memory
 * range it lives in; the GPU driver then removes it (glue/gpu.c). */
#pragma once
#include <linux/lkpi_all.h>

struct pci_dev;
struct platform_device;

int devm_aperture_acquire_for_platform_device(struct platform_device *pdev, resource_size_t base,
					      resource_size_t size);
int aperture_remove_conflicting_devices(resource_size_t base, resource_size_t size, const char *name);
int __aperture_remove_legacy_vga_devices(struct pci_dev *pdev);
int aperture_remove_conflicting_pci_devices(struct pci_dev *pdev, const char *name);
