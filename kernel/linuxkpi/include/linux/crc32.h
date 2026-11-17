#ifndef _LKPI_LINUX_CRC32_H
#define _LKPI_LINUX_CRC32_H
#include <linux/kernel.h>
LKPI_EXTERN_C_BEGIN
u32 crc32_le(u32 crc, const unsigned char* p, size_t len);
u32 crc32_be(u32 crc, const unsigned char* p, size_t len);
#define crc32(seed, data, len) crc32_le(seed, (const unsigned char*)(data), len)
#define ether_crc(len, addr) crc32_be(~0, addr, len)
#define ether_crc_le(len, addr) crc32_le(~0, addr, len)
LKPI_EXTERN_C_END
#endif
