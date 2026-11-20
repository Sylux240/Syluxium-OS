// Boot splash (see splash.hpp). Drawn straight into the firmware
// framebuffer: black, the logo in the middle, an XP-style load bar (three
// blue blocks gliding through a rounded track) and the Esc hint top left.

#include <boot/limine_requests.hpp>
#include <lib/string.hpp>
#include <sched/sched.hpp>
#include <sched/sync.hpp>
#include <time/clock.hpp>
#include <video/console.hpp>
#include <video/font.hpp>
#include <video/framebuffer.hpp>
#include <video/splash.hpp>
#include <fs/vfs.hpp>

namespace splash {

namespace {

video::Framebuffer* g_fb = nullptr;
volatile bool g_active = false;
volatile bool g_verbose = false;  // Esc: show the text console
volatile u64 g_deadline_ms = 0;   // desktop boot: give up waiting for X then
volatile bool g_x_started = false; // X has the display: never switch to text over it
u32 g_logo_x, g_logo_y;
sched::Mutex g_draw;              // one frame at a time; handover waits for it
u32 g_bar_x, g_bar_y;
constexpr u32 BAR_W = 180, BAR_H = 18;
constexpr const char* HINT = "Press Esc for verbose mode";

void text(u32 x, u32 y, const char* s, u32 rgb) {
    const video::Font& f = video::default_font;
    u32 c = g_fb->color(rgb);
    for (; *s; s++, x += f.width) {
        const u8* g = f.glyph(static_cast<u8>(*s));
        for (u32 row = 0; row < f.height; row++)
            for (u32 col = 0; col < 8; col++)
                if (g[row] & (0x80 >> col))
                    g_fb->put_pixel(x + col, y + row, c);
    }
}

// The track: a rounded grey outline, black inside.
void draw_track() {
    u32 grey = g_fb->color(0x9a9a9a), black = g_fb->color(0);
    g_fb->fill_rect(g_bar_x + 2, g_bar_y, BAR_W - 4, BAR_H, grey);
    g_fb->fill_rect(g_bar_x, g_bar_y + 2, BAR_W, BAR_H - 4, grey);
    g_fb->fill_rect(g_bar_x + 1, g_bar_y + 1, BAR_W - 2, BAR_H - 2, grey);
    g_fb->fill_rect(g_bar_x + 2, g_bar_y + 2, BAR_W - 4, BAR_H - 4, black);
}

// One animation frame: blocks at offset `pos` (pixels into the track).
void draw_blocks(int pos) {
    constexpr int BW = 9, GAP = 3, INSET = 4;
    int inner_w = BAR_W - 2 * INSET;
    g_fb->fill_rect(g_bar_x + INSET, g_bar_y + INSET, inner_w, BAR_H - 2 * INSET, g_fb->color(0));
    for (int b = 0; b < 3; b++) {
        int x0 = pos + b * (BW + GAP);
        for (int x = x0; x < x0 + BW; x++) {
            if (x < 0 || x >= inner_w)
                continue;
            // Glossy blue: light top, darker bottom.
            for (u32 y = 0; y < BAR_H - 2 * INSET; y++) {
                u32 rgb = y < 3 ? 0x6fa3ff : y < 6 ? 0x3c78f0 : 0x2552c8;
                g_fb->put_pixel(g_bar_x + INSET + x, g_bar_y + INSET + y, g_fb->color(rgb));
            }
        }
    }
}

void draw_logo() {
    u32 w = g_fb->width(), h = g_fb->height();
    for (u32 y = 0; y < LOGO_H && g_logo_y + y < h; y++)
        for (u32 x = 0; x < LOGO_W && g_logo_x + x < w; x++)
            g_fb->put_pixel(g_logo_x + x, g_logo_y + y, g_fb->color(LOGO[y * LOGO_W + x]));
}

void draw_all() {
    g_fb->fill_rect(0, 0, g_fb->width(), g_fb->height(), g_fb->color(0));
    draw_logo();
    draw_track();
    text(16, 12, HINT, 0x8c8c8c);
}

// Esc / timeout: the log as text - unless X owns the screen by now, then
// the splash just stops (the login screen is on its way).
void dismiss() {
    LockGuard guard(g_draw);
    if (!g_active)
        return;
    g_active = false;
    if (!g_x_started)
        console::set_graphics(false); // repaints everything logged so far, then live
}

void animate(void*) {
    int pos = -40;
    u32 frame = 0;
    while (g_active) {
        if (g_verbose || (g_deadline_ms && clock::now_ms() >= g_deadline_ms)) {
            dismiss();
            return;
        }
        {
            LockGuard guard(g_draw);
            if (!g_active)
                return;
            // While X starts, its first frame may blank the screen: repaint.
            if (g_x_started && frame % 3 == 0) {
                draw_logo();
                draw_track();
                text(16, 12, HINT, 0x8c8c8c);
            }
            draw_blocks(pos);
        }
        frame++;
        pos += 4;
        if (pos > static_cast<int>(BAR_W))
            pos = -40;
        sched::sleep_ms(30);
    }
}

} // namespace

void show(video::Framebuffer& fb) {
    if (!fb.valid() || strstr(boot::cmdline(), "verbose"))
        return;
    g_fb = &fb;
    console::set_graphics(true); // keeps recording, stops drawing
    g_active = true;
    u32 w = fb.width(), h = fb.height();
    // Where Limine's centered background (logo + LOGO_PAD below) put it.
    g_logo_x = w > LOGO_W ? (w - LOGO_W) / 2 : 0;
    g_logo_y = h > LOGO_H + LOGO_PAD ? (h - LOGO_H - LOGO_PAD) / 2 : 0;
    g_bar_x = (w - BAR_W) / 2;
    g_bar_y = g_logo_y + LOGO_H + 50;
    draw_all();
    draw_blocks(-40);
}

void start_animation() {
    if (g_active)
        sched::spawn("splash", animate, nullptr, -1, true);
}

void request_verbose() { g_verbose = true; }

void boot_finished(bool desktop) {
    if (!g_active)
        return;
    if (desktop)
        g_deadline_ms = clock::now_ms() + 90'000; // X should have the screen long before
    else
        g_verbose = true;
}

void handover() {
    // X keeps the splash on screen (started with -background none) until
    // the login screen is ready to draw: just remember not to go to text.
    g_x_started = true;
    if (g_active && !g_deadline_ms)
        g_deadline_ms = clock::now_ms() + 90'000;
}

void finish() {
    LockGuard guard(g_draw); // the current frame finishes first
    if (!g_active)
        return;
    g_active = false;
    if (!g_x_started) // nobody else draws: show the console
        console::set_graphics(false);
}

namespace {
class SplashDevice : public vfs::Inode {
public:
    SplashDevice() {
        ino = vfs::next_ino();
        mode = vfs::S_IFCHR | 0600;
        rdev = (10 << 8) | 250;
        atime = mtime = ctime = vfs::now();
    }
    bool seekable() const override { return false; }
    isize read(u64, void*, usize) override { return 0; }
    isize write(u64, const void*, usize n) override {
        finish();
        return static_cast<isize>(n);
    }
};
} // namespace

void create_device() { vfs::add_node("/dev/splash", new SplashDevice); }

void panic_off() {
    if (!g_active)
        return;
    g_active = false;
    console::set_graphics(false);
}

bool active() { return g_active; }

} // namespace splash
