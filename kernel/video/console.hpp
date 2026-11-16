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
void putc(char c);
void write(const char* s, usize len);
void clear();
u32 rows();
u32 cols();

} // namespace console
