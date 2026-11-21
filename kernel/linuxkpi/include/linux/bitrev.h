/* LinuxKPI: bit reversal (linux/bitrev.h). */
#pragma once
#include <linux/types.h>

static inline u8 bitrev8(u8 b)
{
    b = (u8)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (u8)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return (u8)((b & 0xAA) >> 1 | (b & 0x55) << 1);
}
static inline u16 bitrev16(u16 x) { return (u16)((bitrev8(x & 0xff) << 8) | bitrev8(x >> 8)); }
static inline u32 bitrev32(u32 x) { return ((u32)bitrev16(x & 0xffff) << 16) | bitrev16(x >> 16); }
