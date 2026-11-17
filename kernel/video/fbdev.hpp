#pragma once

// /dev/fb0 (Linux fbdev ABI) on the firmware framebuffer.
namespace fbdev {

void init(); // creates /dev/fb0 if there is a framebuffer

} // namespace fbdev
