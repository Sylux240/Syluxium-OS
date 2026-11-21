#pragma once
// Boot splash (Dusk theme): the Syluxium logo on deep navy with faint
// stars, a cyan progress bar, the current boot stage under it and "Press
// Esc to see boot details". The text console keeps recording underneath; Esc (or the
// boot option "verbose") shows it at once. A graphical program taking the
// screen (X through /dev/fb0, /dev/dri or KDSETMODE) ends the splash quietly.
#include <lib/types.hpp>

namespace video {
class Framebuffer;
}

namespace splash {

// The splash picture (tools/make-boot-logo.py; Limine shows the same one,
// centered): its size, background colour, and where the bar and text go.
extern const u32 PIC_W, PIC_H, PIC_BG, PIC_BAR_W, PIC_BAR_H, PIC_BAR_Y, PIC_TEXT_Y;
extern const u32 PIC[]; // 0xRRGGBB

void show(video::Framebuffer& fb); // right after the console exists
void start_animation();            // once kernel threads run
void stage(u32 percent, const char* text); // boot progress: the bar moves towards percent
void request_verbose();            // Esc pressed (any context)
void boot_finished(bool desktop);  // the console shell starts
void handover();                   // X is starting (it opened the display)
void finish();                     // the login screen is about to draw (/dev/splash)
void create_device();              // /dev/splash
void panic_off();                  // kernel panic: text, immediately
bool active();

} // namespace splash
