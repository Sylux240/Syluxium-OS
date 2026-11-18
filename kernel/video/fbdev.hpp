#pragma once

// /dev/fb0 (Linux fbdev ABI) on the firmware framebuffer.
namespace fbdev {

void init(); // creates /dev/fb0 if there is a framebuffer
// A video mode the firmware supports (recorded at boot, before init()):
// /sys/class/graphics/fb0/modes lists them.
void record_mode(u32 width, u32 height, u16 bpp);

} // namespace fbdev
