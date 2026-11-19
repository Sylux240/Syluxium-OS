/* LinuxKPI: random numbers (RDRAND/TSC mixed by the kernel). */
#ifndef _LKPI_LINUX_RANDOM_H
#define _LKPI_LINUX_RANDOM_H
#include <linux/kernel.h>
LKPI_EXTERN_C_BEGIN
void get_random_bytes(void* buf, size_t len);
u32 get_random_u32(void);
static inline u8 get_random_u8(void) { return (u8)get_random_u32(); }
static inline u16 get_random_u16(void) { return (u16)get_random_u32(); }
static inline u64 get_random_u64(void) { return ((u64)get_random_u32() << 32) | get_random_u32(); }
static inline unsigned long get_random_long(void) { return (unsigned long)get_random_u64(); }
static inline u32 get_random_u32_below(u32 ceil) { return ceil ? (u32)(((u64)get_random_u32() * ceil) >> 32) : 0; }
static inline u32 get_random_u32_inclusive(u32 floor, u32 ceil) { return floor + get_random_u32_below(ceil - floor + 1); }
#define prandom_u32_max get_random_u32_below
#define get_random_int get_random_u32
#define prandom_u32 get_random_u32
#define prandom_bytes get_random_bytes
LKPI_EXTERN_C_END
#endif
