#pragma once
// Boot splash: the Syluxium logo, an animated load bar and "Press Esc for
// verbose mode". The text console keeps recording underneath; Esc (or the
// boot option "verbose") shows it at once. A graphical program taking the
// screen (X through /dev/fb0, /dev/dri or KDSETMODE) ends the splash quietly.
#include <lib/types.hpp>

namespace video {
class Framebuffer;
}

namespace splash {

extern const u32 LOGO_W, LOGO_H, LOGO_PAD; // LOGO_PAD: black below it (Limine background)
extern const u32 LOGO[]; // 0xRRGGBB, generated from the wallpaper

void show(video::Framebuffer& fb); // right after the console exists
void start_animation();            // once kernel threads run
void request_verbose();            // Esc pressed (any context)
void boot_finished(bool desktop);  // the console shell starts
void handover();                   // X is starting (it opened the display)
void finish();                     // the login screen is about to draw (/dev/splash)
void create_device();              // /dev/splash
void panic_off();                  // kernel panic: text, immediately
bool active();

} // namespace splash
