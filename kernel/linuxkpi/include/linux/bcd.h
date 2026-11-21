/* LinuxKPI: binary-coded decimal. */
#pragma once
#include <linux/types.h>
#define bcd2bin(x) ((((u8)(x)) & 0x0f) + (((u8)(x)) >> 4) * 10)
#define bin2bcd(x) ((((x) / 10) << 4) + (x) % 10)
#define bcd_is_valid(x) ((((x) & 0x0f) < 10) && (((x) >> 4) < 10))
