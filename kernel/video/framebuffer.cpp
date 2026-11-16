#include <video/framebuffer.hpp>

namespace video {

bool Framebuffer::init(void* address, u32 width, u32 height, u32 pitch, u16 bpp,
                       u8 red_shift, u8 green_shift, u8 blue_shift) {
    if (!address || bpp != 32)
        return false;
    m_base = static_cast<u32*>(address);
    m_width = width;
    m_height = height;
    m_pitch = pitch;
    m_red_shift = red_shift;
    m_green_shift = green_shift;
    m_blue_shift = blue_shift;
    return true;
}

void Framebuffer::fill_rect(u32 x, u32 y, u32 w, u32 h, u32 native_color) const {
    if (x >= m_width || y >= m_height)
        return;
    u32 x_end = min(x + w, m_width);
    u32 y_end = min(y + h, m_height);
    for (u32 py = y; py < y_end; py++) {
        u32* line = row(py);
        for (u32 px = x; px < x_end; px++)
            line[px] = native_color;
    }
}

Framebuffer& primary_framebuffer() {
    static Framebuffer fb;
    return fb;
}

} // namespace video
