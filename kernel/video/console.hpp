#pragma once

#include <lib/types.hpp>

namespace video {
class Framebuffer;
}

// Text console drawn on the framebuffer. Understands \n \r \t \b and the
// ANSI SGR color escapes (ESC[...m), so log lines look the same on screen
// and on the serial terminal.
namespace console {

void init(video::Framebuffer& fb);
bool ready();
// KD_GRAPHICS / KD_TEXT (KDSETMODE): while in graphics mode the console keeps
// its text in memory but doesn't touch the screen (an X server owns it).
void set_graphics(bool graphics);
bool graphics();
void putc(char c);
void write(const char* s, usize len);
void clear();
u32 rows();
u32 cols();

} // namespace console
