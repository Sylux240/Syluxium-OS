/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
#define pgprot_framebuffer(prot, vm_start, vm_end, offset) pgprot_writecombine(prot)
struct fb_info;
static inline bool fb_is_primary_device(struct fb_info *info) { return true; }
