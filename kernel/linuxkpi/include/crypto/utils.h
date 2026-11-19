/* LinuxKPI: crypto helpers. */
#ifndef _LKPI_CRYPTO_UTILS_H
#define _LKPI_CRYPTO_UTILS_H

#include <linux/kernel.h>

static inline void crypto_xor(u8* dst, const u8* src, unsigned int size) {
    while (size--) *dst++ ^= *src++;
}
static inline void crypto_xor_cpy(u8* dst, const u8* src1, const u8* src2, unsigned int size) {
    while (size--) *dst++ = *src1++ ^ *src2++;
}
/* Constant-time comparison: nonzero if different. */
static inline int crypto_memneq(const void* a, const void* b, size_t size) {
    const u8* x = (const u8*)a;
    const u8* y = (const u8*)b;
    u8 neq = 0;
    while (size--) neq |= *x++ ^ *y++;
    return neq;
}

#endif
