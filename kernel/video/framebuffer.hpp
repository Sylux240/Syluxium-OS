#pragma once

#include <lib/types.hpp>

namespace video {

// Linear 32-bpp framebuffer handed to us by the bootloader (GOP/VBE).
// Later phases replace this with a real display driver + compositor.
class Framebuffer {
public:
    bool init(void* address, u32 width, u32 height, u32 pitch, u16 bpp,
              u8 red_shift, u8 green_shift, u8 blue_shift);

    bool valid() const { return m_base != nullptr; }
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }

    // Convert 0xRRGGBB into the framebuffer's native pixel layout.
    u32 color(u32 rgb) const {
        u32 r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
        return (r << m_red_shift) | (g << m_green_shift) | (b << m_blue_shift);
    }

    u32* row(u32 y) const { return reinterpret_cast<u32*>(reinterpret_cast<u8*>(m_base) + y * m_pitch); }

    void put_pixel(u32 x, u32 y, u32 native_color) const {
        if (x < m_width && y < m_height)
            row(y)[x] = native_color;
    }

    void fill_rect(u32 x, u32 y, u32 w, u32 h, u32 native_color) const;

private:
    u32* m_base = nullptr;
    u32 m_width = 0;
    u32 m_height = 0;
    u32 m_pitch = 0; // bytes per scanline
    u8 m_red_shift = 16;
    u8 m_green_shift = 8;
    u8 m_blue_shift = 0;
};

Framebuffer& primary_framebuffer();

} // namespace video
