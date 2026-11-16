#pragma once

#include <lib/types.hpp>

namespace video {

// Fixed-width 1-bit bitmap font, 8 pixels wide (one byte per row, MSB = leftmost).
struct Font {
    u32 width;
    u32 height;
    const u8* glyphs; // 256 glyphs * height bytes, indexed by code point U+00..U+FF

    const u8* glyph(u8 c) const { return glyphs + static_cast<usize>(c) * height; }
};

extern const Font default_font;

} // namespace video
