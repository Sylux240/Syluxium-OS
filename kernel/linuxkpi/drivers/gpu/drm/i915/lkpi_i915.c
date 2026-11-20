// SPDX-License-Identifier: MIT
/*
 * LinuxKPI: the parts of i915 Syluxium replaces or leaves out.
 *
 *  - graphics stolen memory: Linux finds it in early boot
 *    (arch/x86/kernel/early-quirks.c); here a core initcall reads the same
 *    PCI registers of the integrated GPU before i915 probes
 *  - user mappings of GTT/LMEM ranges (i915_mm.c): through the LinuxKPI
 *    mapping table (remap_io_mapping / remap_io_sg)
 *  - left out (stubs): perf/OA streams, sysfs and debugfs files, userptr
 *    objects, the private huge-page tmpfs (gemfs), the SSE4.1 WC memcpy,
 *    pre-gen6 GMCH GTT support
 */
#include <linux/pci.h>
#include <drm/intel/i915_pciids.h>

#include "i915_drv.h"
#include "i915_memcpy.h"
#include "i915_mm.h"
#include "i915_perf.h"
#include "i915_sysfs.h"
#include "gem/i915_gemfs.h"
#include "gem/i915_gem_ioctls.h"
#include "gt/intel_ggtt_gmch.h"
#include "gt/intel_gt_debugfs.h"
#include "gt/intel_gt_sysfs.h"

/* ---- stolen memory (early-quirks.c) ---- */
struct resource intel_graphics_stolen_res = DEFINE_RES_MEM(0, 0);

enum { STOLEN_GEN6 = 1, STOLEN_GEN8, STOLEN_CHV, STOLEN_GEN9, STOLEN_GEN11 };
static const struct pci_device_id stolen_ids[] = {
	INTEL_SNB_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN6),
	INTEL_IVB_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN6),
	INTEL_HSW_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN6),
	INTEL_VLV_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN6),
	INTEL_BDW_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN8),
	INTEL_CHV_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_CHV),
	INTEL_SKL_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN9),
	INTEL_BXT_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN9),
	INTEL_KBL_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN9),
	INTEL_CFL_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN9),
	INTEL_WHL_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN9),
	INTEL_CML_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN9),
	INTEL_GLK_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN9),
	INTEL_CNL_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN9),
	INTEL_ICL_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN11),
	INTEL_EHL_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN11),
	INTEL_JSL_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN11),
	INTEL_TGL_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN11),
	INTEL_RKL_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN11),
	INTEL_ADLS_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN11),
	INTEL_ADLP_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN11),
	INTEL_ADLN_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN11),
	INTEL_RPLS_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN11),
	INTEL_RPLU_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN11),
	INTEL_RPLP_IDS(INTEL_VGA_DEVICE, (void *)STOLEN_GEN11),
	{ }
};

static int __init lkpi_i915_stolen_quirk(void)
{
	struct pci_dev *pdev = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(2, 0));
	const struct pci_device_id *id;
	u16 gmch_ctrl;
	u32 bsm, gms;
	u64 base, size;

	if (!pdev || pdev->vendor != PCI_VENDOR_ID_INTEL)
		return 0;
	id = pci_match_id(stolen_ids, pdev);
	if (!id)
		return 0;
	pci_read_config_word(pdev, 0x50, &gmch_ctrl); /* SNB_GMCH_CTRL */
	switch (id->driver_data) {
	case STOLEN_GEN6:
		size = (u64)((gmch_ctrl >> 3) & 0x1f) * SZ_32M;
		break;
	case STOLEN_GEN8:
		size = (u64)((gmch_ctrl >> 8) & 0xff) * SZ_32M;
		break;
	case STOLEN_CHV:
		gms = (gmch_ctrl >> 3) & 0x1f;
		size = gms < 0x11 ? (u64)gms * SZ_32M : gms < 0x17 ? (u64)(gms - 0x11) * SZ_4M + SZ_8M : (u64)(gms - 0x17) * SZ_4M + (36ULL << 20);
		break;
	default: /* gen9+ */
		gms = (gmch_ctrl >> 8) & 0xff;
		size = gms < 0xf0 ? (u64)gms * SZ_32M : (u64)(gms - 0xf0 + 1) * SZ_4M;
		break;
	}
	if (id->driver_data == STOLEN_GEN11) {
		u32 lo, hi;
		pci_read_config_dword(pdev, 0xc0, &lo); /* INTEL_GEN11_BSM_DW0/1 */
		pci_read_config_dword(pdev, 0xc4, &hi);
		base = ((u64)hi << 32) | (lo & 0xfff00000);
	} else {
		pci_read_config_dword(pdev, 0x5c, &bsm); /* INTEL_BSM */
		base = bsm & 0xfff00000;
	}
	if (!size || !base)
		return 0;
	intel_graphics_stolen_res = (struct resource)DEFINE_RES_MEM(base, size);
	pr_info("i915: graphics stolen memory %#llx-%#llx (%llu MiB)\n", base, base + size - 1, size >> 20);
	return 0;
}
core_initcall(lkpi_i915_stolen_quirk);

/* ---- user mappings (i915_mm.c) ---- */
int lkpi_vma_insert_range(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn,
			  unsigned long npages, pgprot_t prot); /* glue/file.c */

int remap_io_mapping(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, unsigned long size,
		     struct io_mapping *iomap)
{
	return lkpi_vma_insert_range(vma, addr, pfn, size >> PAGE_SHIFT, iomap->prot);
}

int remap_io_sg(struct vm_area_struct *vma, unsigned long addr, unsigned long size, struct scatterlist *sgl,
		unsigned long offset, resource_size_t iobase)
{
	unsigned long end = addr + size;
	struct scatterlist *sg = sgl;
	pgprot_t prot = iobase == -1 ? vma->vm_page_prot : pgprot_writecombine(vma->vm_page_prot);

	offset <<= PAGE_SHIFT; /* offset is in pages */
	while (sg && offset >= (iobase == -1 ? sg->length : sg_dma_len(sg))) {
		offset -= iobase == -1 ? sg->length : sg_dma_len(sg);
		sg = sg_next(sg);
	}
	for (; sg && addr < end; sg = sg_next(sg), offset = 0) {
		unsigned long len = iobase == -1 ? sg->length : sg_dma_len(sg);
		u64 start = iobase == -1 ? page_to_phys(sg_page(sg)) : iobase + sg_dma_address(sg);
		unsigned long n = min_t(unsigned long, (len - offset) >> PAGE_SHIFT, (end - addr) >> PAGE_SHIFT);
		int r;
		if (!len)
			break;
		r = lkpi_vma_insert_range(vma, addr, (start + offset) >> PAGE_SHIFT, n, prot);
		if (r)
			return r;
		addr += n << PAGE_SHIFT;
	}
	return 0;
}

/* ---- left out ---- */
void i915_memcpy_init_early(struct drm_i915_private *i915) { }
bool i915_memcpy_from_wc(void *dst, const void *src, unsigned long len) { return false; }
void i915_unaligned_memcpy_from_wc(void *dst, const void *src, unsigned long len) { memcpy(dst, src, len); }

void i915_gemfs_init(struct drm_i915_private *i915) { }
void i915_gemfs_fini(struct drm_i915_private *i915) { }

int i915_gem_userptr_ioctl(struct drm_device *dev, void *data, struct drm_file *file) { return -ENODEV; }

void intel_ggtt_gmch_flush(void) { }
int intel_ggtt_gmch_enable_hw(struct drm_i915_private *i915) { return -ENODEV; }
int intel_ggtt_gmch_probe(struct i915_ggtt *ggtt) { return -ENODEV; }

void i915_setup_sysfs(struct drm_i915_private *i915) { }
void i915_teardown_sysfs(struct drm_i915_private *i915) { }
void intel_gt_sysfs_register(struct intel_gt *gt) { }
void intel_gt_sysfs_unregister(struct intel_gt *gt) { }
void intel_gt_debugfs_register(struct intel_gt *gt) { }

int i915_perf_init(struct drm_i915_private *i915) { return 0; }
void i915_perf_fini(struct drm_i915_private *i915) { }
void i915_perf_register(struct drm_i915_private *i915) { }
void i915_perf_unregister(struct drm_i915_private *i915) { }
int i915_perf_ioctl_version(struct drm_i915_private *i915) { return 0; }
int i915_perf_sysctl_register(void) { return 0; }
void i915_perf_sysctl_unregister(void) { }
int i915_perf_open_ioctl(struct drm_device *dev, void *data, struct drm_file *file) { return -ENOTSUPP; }
int i915_perf_add_config_ioctl(struct drm_device *dev, void *data, struct drm_file *file) { return -ENOTSUPP; }
int i915_perf_remove_config_ioctl(struct drm_device *dev, void *data, struct drm_file *file) { return -ENOTSUPP; }
void i915_oa_init_reg_state(const struct intel_context *ce, const struct intel_engine_cs *engine) { }
struct i915_oa_config *i915_perf_get_oa_config(struct i915_perf *perf, int metrics_set) { return NULL; }
void i915_oa_config_release(struct kref *ref) { }
u32 i915_perf_oa_timestamp_frequency(struct drm_i915_private *i915) { return 0; }

/* MIPI DSI panels (tablets, some Atom laptops): no DSI bus here. */
#include <drm/drm_mipi_dsi.h>
#include <video/mipi_display.h>
bool mipi_dsi_packet_format_is_long(u8 type)
{
	switch (type) {
	case MIPI_DSI_NULL_PACKET: case MIPI_DSI_BLANKING_PACKET: case MIPI_DSI_GENERIC_LONG_WRITE:
	case MIPI_DSI_DCS_LONG_WRITE: case MIPI_DSI_PICTURE_PARAMETER_SET: case MIPI_DSI_COMPRESSED_PIXEL_STREAM:
	case MIPI_DSI_LOOSELY_PACKED_PIXEL_STREAM_YCBCR20: case MIPI_DSI_PACKED_PIXEL_STREAM_YCBCR24:
	case MIPI_DSI_PACKED_PIXEL_STREAM_YCBCR16: case MIPI_DSI_PACKED_PIXEL_STREAM_30:
	case MIPI_DSI_PACKED_PIXEL_STREAM_36: case MIPI_DSI_PACKED_PIXEL_STREAM_YCBCR12:
	case MIPI_DSI_PACKED_PIXEL_STREAM_16: case MIPI_DSI_PACKED_PIXEL_STREAM_18:
	case MIPI_DSI_PIXEL_STREAM_3BYTE_18: case MIPI_DSI_PACKED_PIXEL_STREAM_24:
		return true;
	}
	return false;
}
int mipi_dsi_create_packet(struct mipi_dsi_packet *packet, const struct mipi_dsi_msg *msg) { return -ENODEV; }
int mipi_dsi_attach(struct mipi_dsi_device *dsi) { return -ENODEV; }
int mipi_dsi_set_maximum_return_packet_size(struct mipi_dsi_device *dsi, u16 value) { return -ENODEV; }
int mipi_dsi_compression_mode(struct mipi_dsi_device *dsi, bool enable) { return -ENODEV; }
int mipi_dsi_picture_parameter_set(struct mipi_dsi_device *dsi, const struct drm_dsc_picture_parameter_set *pps) { return -ENODEV; }
ssize_t mipi_dsi_generic_write(struct mipi_dsi_device *dsi, const void *payload, size_t size) { return -ENODEV; }
ssize_t mipi_dsi_dcs_write_buffer(struct mipi_dsi_device *dsi, const void *data, size_t len) { return -ENODEV; }
ssize_t mipi_dsi_dcs_write(struct mipi_dsi_device *dsi, u8 cmd, const void *data, size_t len) { return -ENODEV; }
ssize_t mipi_dsi_dcs_read(struct mipi_dsi_device *dsi, u8 cmd, void *data, size_t len) { return -ENODEV; }
int mipi_dsi_dcs_nop(struct mipi_dsi_device *dsi) { return -ENODEV; }
