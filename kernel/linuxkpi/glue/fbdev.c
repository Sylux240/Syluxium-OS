/* LinuxKPI: the fbdev core, as far as DRM's fbdev emulation needs it.
 *
 * A GPU driver that takes the display over (glue/gpu.c) lights the screen
 * up through DRM's fbdev emulation (drm_fb_helper: i915's intel_fbdev,
 * drm_fbdev_ttm, ...), which registers a framebuffer here. Linux would put
 * fbcon on it; Syluxium moves its own kernel console there instead, so boot
 * messages (and panics) stay visible until X takes the screen, and come back
 * if X fails. Framebuffers with a shadow buffer (deferred I/O: TTM drivers)
 * are flushed to the GPU buffer 10 times a second while the console shows;
 * direct ones get an empty fill so the driver knows the CPU draws there. */
#include <linux/lkpi_all.h>
#include <linux/fb.h>

void lkpi_console_attach(void *vaddr, u32 width, u32 height, u32 pitch, u32 red_shift, u32 green_shift, u32 blue_shift);
void lkpi_console_detach(void *vaddr);
bool lkpi_console_text_mode(void);
bool lkpi_boot_fb_taken_by_gpu(void);
bool lkpi_boot_fb_owner(struct device *dev);

static struct fb_info *console_info;
static struct delayed_work console_flush_work;
static bool flush_ready;

struct fb_info *framebuffer_alloc(size_t size, struct device *dev)
{
	struct fb_info *info = kzalloc(sizeof(*info) + size, GFP_KERNEL);
	if (!info)
		return NULL;
	if (size)
		info->par = info + 1;
	info->device = dev;
	return info;
}

void framebuffer_release(struct fb_info *info)
{
	kfree(info);
}

/* A shadow-buffered framebuffer: tell the driver the whole screen changed. */
unsigned long long lkpi_console_changes(void); /* core.cpp */
static void flush_fn(struct work_struct *w)
{
	static unsigned long long flushed = ~0ull;
	struct fb_info *info = READ_ONCE(console_info);
	unsigned long long changes = lkpi_console_changes();
	/* Only when the console drew: every flush is a display commit (amdgpu
	 * runs a full atomic check for it). */
	if (changes == flushed) {
		if (info)
			schedule_delayed_work(&console_flush_work, msecs_to_jiffies(100));
		return;
	}
	flushed = changes;
	if (info && info->fbdefio && info->fbdefio->deferred_io && lkpi_console_text_mode()) {
		struct fb_deferred_io_pageref first = { 0 }, last = { 0 };
		LIST_HEAD(pages);
		first.offset = 0;
		last.offset = info->fix.smem_len > PAGE_SIZE ? PAGE_ALIGN_DOWN(info->fix.smem_len - 1) : 0;
		list_add_tail(&first.list, &pages);
		list_add_tail(&last.list, &pages);
		info->fbdefio->deferred_io(info, &pages);
	} else if (info && info->fbops && info->fbops->fb_fillrect && lkpi_console_text_mode()) {
		/* Direct framebuffers: an empty fill through the driver tells it the
		 * CPU draws there (i915 leaves panel self refresh and FBC). */
		struct fb_fillrect none = { 0 };
		info->fbops->fb_fillrect(info, &none);
	}
	if (READ_ONCE(console_info))
		schedule_delayed_work(&console_flush_work, msecs_to_jiffies(100));
}

int register_framebuffer(struct fb_info *info)
{
	void *vaddr = info->fbdefio ? (void *)info->screen_buffer : (void __force *)info->screen_base;
	struct fb_var_screeninfo *var = &info->var;

	printk(KERN_INFO "fbdev: %s: %ux%u, %u bpp, pitch %u%s\n", info->fix.id, var->xres, var->yres,
	       var->bits_per_pixel, info->fix.line_length, info->fbdefio ? ", shadow buffer" : "");
	/* simpledrm's emulation shows the firmware framebuffer the console
	 * already draws on; a GPU driver's replaces it. */
	if (!lkpi_boot_fb_taken_by_gpu() || !vaddr || var->bits_per_pixel != 32)
		return 0;
	/* A second GPU (no boot screen on it) keeps its framebuffer to itself:
	 * the console stays where the monitor is. */
	if (!lkpi_boot_fb_owner(info->device)) {
		printk(KERN_INFO "fbdev: %s: not the boot display's GPU: the console stays where it is\n", info->fix.id);
		return 0;
	}
	console_info = info;
	/* What fbcon does when it takes a framebuffer over: set the mode, so the
	 * screen scans out this framebuffer (DRM's fbdev emulation does no mode
	 * set of its own; without this the screen kept showing the firmware
	 * picture until a program opened and closed /dev/dri). */
	if (info->fbops && info->fbops->fb_set_par) {
		int r = info->fbops->fb_set_par(info);
		if (r)
			printk(KERN_WARNING "fbdev: %s: setting the display mode failed (%d)\n", info->fix.id, r);
	}
	lkpi_console_attach(vaddr, var->xres, var->yres, info->fix.line_length, var->red.offset, var->green.offset,
			    var->blue.offset);
	if (!flush_ready) {
		INIT_DELAYED_WORK(&console_flush_work, flush_fn);
		flush_ready = true;
	}
	schedule_delayed_work(&console_flush_work, msecs_to_jiffies(100));
	return 0;
}

void unregister_framebuffer(struct fb_info *info)
{
	if (console_info != info)
		return;
	WRITE_ONCE(console_info, NULL);
	lkpi_console_detach(info->fbdefio ? (void *)info->screen_buffer : (void __force *)info->screen_base);
	if (flush_ready)
		cancel_delayed_work_sync(&console_flush_work);
}

void fb_set_suspend(struct fb_info *info, int state) { }

int fb_alloc_cmap(struct fb_cmap *cmap, int len, int transp)
{
	int size = len * sizeof(u16);
	cmap->red = kzalloc(size, GFP_KERNEL);
	cmap->green = kzalloc(size, GFP_KERNEL);
	cmap->blue = kzalloc(size, GFP_KERNEL);
	cmap->transp = transp ? kzalloc(size, GFP_KERNEL) : NULL;
	if (!cmap->red || !cmap->green || !cmap->blue || (transp && !cmap->transp)) {
		fb_dealloc_cmap(cmap);
		return -ENOMEM;
	}
	cmap->start = 0;
	cmap->len = len;
	return 0;
}

void fb_dealloc_cmap(struct fb_cmap *cmap)
{
	kfree(cmap->red);
	kfree(cmap->green);
	kfree(cmap->blue);
	kfree(cmap->transp);
	cmap->red = cmap->green = cmap->blue = cmap->transp = NULL;
	cmap->len = 0;
}

/* Deferred I/O: flushed by flush_fn above, no mmap of /dev/fb here. */
int fb_deferred_io_init(struct fb_info *info)
{
	if (info->fbdefio) {
		mutex_init(&info->fbdefio->lock);
		INIT_LIST_HEAD(&info->fbdefio->pagereflist);
	}
	return 0;
}
void fb_deferred_io_cleanup(struct fb_info *info) { }
int fb_deferred_io_mmap(struct fb_info *info, struct vm_area_struct *vma) { return -ENODEV; }

/* /dev/fbN reads and writes: there is no fbdev device node for these. */
ssize_t fb_io_read(struct fb_info *info, char __user *buf, size_t count, loff_t *ppos) { return -ENODEV; }
ssize_t fb_io_write(struct fb_info *info, const char __user *buf, size_t count, loff_t *ppos) { return -ENODEV; }
ssize_t fb_sys_read(struct fb_info *info, char __user *buf, size_t count, loff_t *ppos) { return -ENODEV; }
ssize_t fb_sys_write(struct fb_info *info, const char __user *buf, size_t count, loff_t *ppos) { return -ENODEV; }

/* Drawing (fbcon's): 32 bpp fills and copies; glyph blits are fbcon only. */
static void fill32(struct fb_info *info, void *base, const struct fb_fillrect *r)
{
	for (u32 y = r->dy; y < r->dy + r->height && y < info->var.yres; y++) {
		u32 *line = (u32 *)((char *)base + (size_t)y * info->fix.line_length);
		for (u32 x = r->dx; x < r->dx + r->width && x < info->var.xres; x++)
			line[x] = r->color;
	}
}
static void copy32(struct fb_info *info, void *base, const struct fb_copyarea *a)
{
	size_t pitch = info->fix.line_length;
	if (a->dy <= a->sy) {
		for (u32 i = 0; i < a->height; i++)
			memmove((char *)base + (a->dy + i) * pitch + a->dx * 4, (char *)base + (a->sy + i) * pitch + a->sx * 4, a->width * 4);
	} else {
		for (u32 i = a->height; i--; )
			memmove((char *)base + (a->dy + i) * pitch + a->dx * 4, (char *)base + (a->sy + i) * pitch + a->sx * 4, a->width * 4);
	}
}
void cfb_fillrect(struct fb_info *info, const struct fb_fillrect *r) { if (info->var.bits_per_pixel == 32) fill32(info, (void __force *)info->screen_base, r); }
void cfb_copyarea(struct fb_info *info, const struct fb_copyarea *a) { if (info->var.bits_per_pixel == 32) copy32(info, (void __force *)info->screen_base, a); }
void cfb_imageblit(struct fb_info *info, const struct fb_image *image) { }
void sys_fillrect(struct fb_info *info, const struct fb_fillrect *r) { if (info->var.bits_per_pixel == 32) fill32(info, info->screen_buffer, r); }
void sys_copyarea(struct fb_info *info, const struct fb_copyarea *a) { if (info->var.bits_per_pixel == 32) copy32(info, info->screen_buffer, a); }
void sys_imageblit(struct fb_info *info, const struct fb_image *image) { }
int fb_io_mmap(struct fb_info *info, struct vm_area_struct *vma) { return -ENODEV; }

/* ---- backlight class devices: kept for the driver, not exposed yet ---- */
#include <linux/backlight.h>
struct backlight_device *backlight_device_register(const char *name, struct device *parent, void *devdata,
						   const struct backlight_ops *ops,
						   const struct backlight_properties *props)
{
	struct backlight_device *bd = kzalloc(sizeof(*bd), GFP_KERNEL);
	if (!bd)
		return ERR_PTR(-ENOMEM);
	mutex_init(&bd->update_lock);
	mutex_init(&bd->ops_lock);
	bd->ops = ops;
	if (props)
		bd->props = *props;
	bd->dev.parent = parent;
	dev_set_drvdata(&bd->dev, devdata);
	dev_set_name(&bd->dev, "%s", name);
	printk(KERN_INFO "backlight: %s registered (brightness %d of %d)\n", name, bd->props.brightness,
	       bd->props.max_brightness);
	return bd;
}

void backlight_device_unregister(struct backlight_device *bd)
{
	if (!IS_ERR_OR_NULL(bd))
		kfree(bd);
}
