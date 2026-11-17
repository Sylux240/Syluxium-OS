// /dev/fb0: the Linux framebuffer device (fbdev) on top of the firmware
// framebuffer. Xorg's fbdev driver, SDL and simple graphics programs draw
// into it by mmap()ing the screen memory.
//
// The mode is whatever the firmware set up; FBIOPUT_VSCREENINFO accepts
// only that mode. Real mode setting comes with DRM/KMS drivers.

#include <fs/vfs.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/layout.hpp>
#include <proc/uaccess.hpp>
#include <video/console.hpp>
#include <video/fbdev.hpp>
#include <video/framebuffer.hpp>

namespace fbdev {

namespace {

constexpr u64 FBIOGET_VSCREENINFO = 0x4600, FBIOPUT_VSCREENINFO = 0x4601, FBIOGET_FSCREENINFO = 0x4602,
              FBIOGETCMAP = 0x4604, FBIOPUTCMAP = 0x4605, FBIOPAN_DISPLAY = 0x4606, FBIOBLANK = 0x4611,
              FBIO_WAITFORVSYNC = 0x40044620;
constexpr u32 FB_TYPE_PACKED_PIXELS = 0, FB_VISUAL_TRUECOLOR = 2;

struct Bitfield {
    u32 offset, length, msb_right;
};

struct VarScreenInfo {
    u32 xres, yres, xres_virtual, yres_virtual, xoffset, yoffset, bits_per_pixel, grayscale;
    Bitfield red, green, blue, transp;
    u32 nonstd, activate, height, width, accel_flags;
    u32 pixclock, left_margin, right_margin, upper_margin, lower_margin, hsync_len, vsync_len, sync, vmode, rotate,
        colorspace;
    u32 reserved[4];
};
static_assert(sizeof(VarScreenInfo) == 160);

struct FixScreenInfo {
    char id[16];
    unsigned long smem_start;
    u32 smem_len, type, type_aux, visual;
    u16 xpanstep, ypanstep, ywrapstep;
    u32 line_length;
    unsigned long mmio_start;
    u32 mmio_len, accel;
    u16 capabilities;
    u16 reserved[2];
};
static_assert(sizeof(FixScreenInfo) == 80);

class FbDevice : public vfs::Inode {
public:
    explicit FbDevice(video::Framebuffer& fb) : m_fb(fb) {
        ino = vfs::next_ino();
        mode = vfs::S_IFCHR | 0660;
        rdev = (29u << 8) | 0; // major 29, minor 0, as on Linux
        atime = mtime = ctime = vfs::now();
        m_phys = mm::direct_virt_to_phys(fb.base());
        m_bytes = static_cast<u64>(fb.pitch()) * fb.height();
        size = m_bytes;
    }

    VarScreenInfo var() const {
        VarScreenInfo v{};
        v.xres = v.xres_virtual = m_fb.width();
        v.yres = v.yres_virtual = m_fb.height();
        v.bits_per_pixel = 32;
        v.red = Bitfield{m_fb.red_shift(), 8, 0};
        v.green = Bitfield{m_fb.green_shift(), 8, 0};
        v.blue = Bitfield{m_fb.blue_shift(), 8, 0};
        v.transp = Bitfield{0, 0, 0};
        v.height = v.width = 0xFFFFFFFF; // physical size unknown
        v.pixclock = 10000;              // ~100 MHz; fbdev users only need it non-zero
        v.vmode = 0;
        return v;
    }

    long ioctl(u64 request, u64 arg) override {
        void* uarg = reinterpret_cast<void*>(arg);
        switch (request) {
        case FBIOGET_VSCREENINFO:
            return uaccess::put(static_cast<VarScreenInfo*>(uarg), var());
        case FBIOPUT_VSCREENINFO: {
            VarScreenInfo want;
            if (long r = uaccess::get(&want, static_cast<VarScreenInfo*>(uarg)); r < 0)
                return r;
            VarScreenInfo cur = var();
            // Only the firmware's mode exists; report it back (as Linux's efifb/simplefb do).
            if (want.xres > cur.xres || want.yres > cur.yres || (want.bits_per_pixel && want.bits_per_pixel != 32))
                return -err::INVAL;
            return uaccess::put(static_cast<VarScreenInfo*>(uarg), cur);
        }
        case FBIOGET_FSCREENINFO: {
            FixScreenInfo f{};
            snprintf(f.id, sizeof(f.id), "Syluxium FB");
            f.smem_start = m_phys;
            f.smem_len = static_cast<u32>(m_bytes);
            f.type = FB_TYPE_PACKED_PIXELS;
            f.visual = FB_VISUAL_TRUECOLOR;
            f.line_length = m_fb.pitch();
            return uaccess::put(static_cast<FixScreenInfo*>(uarg), f);
        }
        case FBIOPAN_DISPLAY:
        case FBIOGETCMAP:
        case FBIOPUTCMAP:
        case FBIOBLANK:
        case FBIO_WAITFORVSYNC:
            return 0;
        default:
            return -err::NOTTY;
        }
    }

    // mmap(): the screen memory itself.
    long mmap_phys(u64 offset, u64* phys) override {
        if (offset >= align_up<u64>(m_bytes, 4096))
            return -err::NXIO;
        *phys = m_phys + offset;
        return 0;
    }

    isize read(u64 offset, void* buf, usize count) override {
        if (offset >= m_bytes)
            return 0;
        usize n = min<u64>(count, m_bytes - offset);
        memcpy(buf, static_cast<u8*>(m_fb.base()) + offset, n);
        return static_cast<isize>(n);
    }

    isize write(u64 offset, const void* buf, usize count) override {
        if (offset >= m_bytes)
            return -err::NOSPC;
        usize n = min<u64>(count, m_bytes - offset);
        memcpy(static_cast<u8*>(m_fb.base()) + offset, buf, n);
        return static_cast<isize>(n);
    }

private:
    video::Framebuffer& m_fb;
    u64 m_phys = 0, m_bytes = 0;
};

} // namespace

void init() {
    video::Framebuffer& fb = video::primary_framebuffer();
    if (!fb.valid())
        return;
    vfs::add_node("/dev/fb0", new FbDevice(fb));
}

} // namespace fbdev
