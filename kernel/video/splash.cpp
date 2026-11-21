// Boot splash (see splash.hpp). Drawn straight into the firmware
// framebuffer: the Dusk picture centered on navy (exactly where Limine put
// it), a thin cyan progress bar with a soft glow that moves towards the
// current boot stage, the stage's name under it, and the Esc hint at the
// bottom.

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
u32 g_pic_x, g_pic_y;
sched::Mutex g_draw;              // one frame at a time; handover waits for it
constexpr const char* HINT = "Press Esc to see boot details";
constexpr u32 CYAN = 0x6fe3f0, MUTED = 0xaab3d6, HINT_COLOR = 0x8a93bd;

volatile u32 g_target = 8;        // percent the bar moves towards
u32 g_shown = 0;                  // percent drawn, in 1/16 steps (smooth)
const char* volatile g_text = "Loading kernel";
const char* g_text_drawn = nullptr;

u32 mix(u32 a, u32 b, u32 t256) { // a -> b by t/256
    u32 r = ((a >> 16 & 255) * (256 - t256) + (b >> 16 & 255) * t256) >> 8;
    u32 g = ((a >> 8 & 255) * (256 - t256) + (b >> 8 & 255) * t256) >> 8;
    u32 bl = ((a & 255) * (256 - t256) + (b & 255) * t256) >> 8;
    return r << 16 | g << 8 | bl;
}

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

void text_centered(u32 cy, const char* s, u32 rgb, u32 clear_w) {
    const video::Font& f = video::default_font;
    u32 w = g_fb->width();
    u32 cx = clear_w < w ? (w - clear_w) / 2 : 0;
    g_fb->fill_rect(cx, cy, clear_w < w ? clear_w : w, f.height, g_fb->color(PIC_BG));
    u32 tw = static_cast<u32>(strlen(s)) * f.width;
    text(tw < w ? (w - tw) / 2 : 0, cy, s, rgb);
}

void draw_picture() {
    u32 w = g_fb->width(), h = g_fb->height();
    for (u32 y = 0; y < PIC_H && g_pic_y + y < h; y++)
        for (u32 x = 0; x < PIC_W && g_pic_x + x < w; x++)
            g_fb->put_pixel(g_pic_x + x, g_pic_y + y, g_fb->color(PIC[y * PIC_W + x]));
}

// The bar at `filled` pixels: track (cyan at 14%), the fill in cyan with a
// glow a few pixels above and below.
void draw_bar(u32 filled) {
    u32 x0 = g_pic_x + (PIC_W - PIC_BAR_W) / 2, y0 = g_pic_y + PIC_BAR_Y;
    constexpr u32 GLOW = 5;
    u32 track = mix(PIC_BG, CYAN, 36);
    for (u32 gy = 0; gy < GLOW; gy++) { // glow rows, fading outwards
        u32 t = 70 - gy * 14;
        u32 c = g_fb->color(mix(PIC_BG, CYAN, t));
        u32 bg = g_fb->color(PIC_BG);
        g_fb->fill_rect(x0, y0 - 1 - gy, filled, 1, c);
        g_fb->fill_rect(x0, y0 + PIC_BAR_H + gy, filled, 1, c);
        if (filled < PIC_BAR_W) {
            g_fb->fill_rect(x0 + filled, y0 - 1 - gy, PIC_BAR_W - filled, 1, bg);
            g_fb->fill_rect(x0 + filled, y0 + PIC_BAR_H + gy, PIC_BAR_W - filled, 1, bg);
        }
    }
    g_fb->fill_rect(x0, y0, filled, PIC_BAR_H, g_fb->color(CYAN));
    if (filled < PIC_BAR_W)
        g_fb->fill_rect(x0 + filled, y0, PIC_BAR_W - filled, PIC_BAR_H, g_fb->color(track));
    // rounded ends: the corner pixels in the background
    u32 bg = g_fb->color(PIC_BG);
    g_fb->put_pixel(x0, y0, bg);
    g_fb->put_pixel(x0, y0 + PIC_BAR_H - 1, bg);
    g_fb->put_pixel(x0 + PIC_BAR_W - 1, y0, bg);
    g_fb->put_pixel(x0 + PIC_BAR_W - 1, y0 + PIC_BAR_H - 1, bg);
}

void draw_status() {
    const char* t = g_text;
    text_centered(g_pic_y + PIC_TEXT_Y, t, MUTED, PIC_W);
    g_text_drawn = t;
}

void draw_hint() {
    u32 h = g_fb->height();
    text_centered(h > 50 ? h - 50 : 0, HINT, HINT_COLOR, 400);
}

void draw_all() {
    g_fb->fill_rect(0, 0, g_fb->width(), g_fb->height(), g_fb->color(PIC_BG));
    draw_picture();
    draw_bar(g_shown * PIC_BAR_W / 1600);
    draw_status();
    draw_hint();
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
            if (g_x_started && frame % 10 == 0)
                draw_all();
            // Ease towards the target; creep slowly past it (but never to
            // the end) so a long stage still shows movement.
            u32 target = g_target * 16;
            if (g_shown < target)
                g_shown += (target - g_shown + 7) / 8;
            else if (g_shown < 96 * 16 && frame % 6 == 0 && g_shown < target + 6 * 16)
                g_shown++;
            draw_bar(g_shown * PIC_BAR_W / 1600);
            if (g_text_drawn != g_text)
                draw_status();
        }
        frame++;
        sched::sleep_ms(40);
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
    // Where Limine's centered background put the picture.
    g_pic_x = w > PIC_W ? (w - PIC_W) / 2 : 0;
    g_pic_y = h > PIC_H ? (h - PIC_H) / 2 : 0;
    g_shown = 4 * 16;
    draw_all();
}

void start_animation() {
    if (g_active)
        sched::spawn("splash", animate, nullptr, -1, true);
}

void stage(u32 percent, const char* text) {
    if (percent > g_target)
        g_target = percent > 100 ? 100 : percent;
    if (text)
        g_text = text;
}

void request_verbose() { g_verbose = true; }

void boot_finished(bool desktop) {
    if (!g_active)
        return;
    if (desktop) {
        stage(82, "Starting desktop");
        g_deadline_ms = clock::now_ms() + 90'000; // X should have the screen long before
    } else {
        g_verbose = true;
    }
}

void handover() {
    // X keeps the splash on screen (started with -background none) until
    // the login screen is ready to draw: just remember not to go to text.
    g_x_started = true;
    stage(94, nullptr);
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
        mode = vfs::S_IFCHR | 0622; // the desktop session (Wayland shell) ends the splash too
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
