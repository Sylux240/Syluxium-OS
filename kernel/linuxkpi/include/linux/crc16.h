/* LinuxKPI: CRC-16 (polynomial 0x8005, reflected: "ARC"). */
#pragma once
#include <linux/types.h>
static inline u16 crc16_byte(u16 crc, const u8 data)
{
    crc ^= data;
    for (int b = 0; b < 8; b++)
        crc = (crc & 1) ? (u16)((crc >> 1) ^ 0xA001) : (u16)(crc >> 1);
    return crc;
}
static inline u16 crc16(u16 crc, const u8 *buffer, size_t len)
{
    while (len--)
        crc = crc16_byte(crc, *buffer++);
    return crc;
}
