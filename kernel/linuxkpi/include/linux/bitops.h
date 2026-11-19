#ifndef _LKPI_LINUX_BITOPS_H
#define _LKPI_LINUX_BITOPS_H

#include <linux/kernel.h>

LKPI_EXTERN_C_BEGIN

#define BIT(n) (1UL << (n))
#define BIT_ULL(n) (1ULL << (n))
#define BIT_MASK(n) (1UL << ((n) % BITS_PER_LONG))
#define BIT_WORD(n) ((n) / BITS_PER_LONG)
#define GENMASK(h, l) (((~0UL) - (1UL << (l)) + 1) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#define GENMASK_ULL(h, l) (((~0ULL) - (1ULL << (l)) + 1) & (~0ULL >> (63 - (h))))
#define BITS_TO_LONGS(n) DIV_ROUND_UP(n, BITS_PER_LONG)
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]

static inline void set_bit(long nr, volatile unsigned long* a) { __atomic_or_fetch(&a[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST); }
static inline void clear_bit(long nr, volatile unsigned long* a) { __atomic_and_fetch(&a[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_SEQ_CST); }
static inline void change_bit(long nr, volatile unsigned long* a) { __atomic_xor_fetch(&a[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST); }
static inline int test_bit(long nr, const volatile unsigned long* a) { return (a[BIT_WORD(nr)] >> (nr % BITS_PER_LONG)) & 1; }
static inline int test_and_set_bit(long nr, volatile unsigned long* a) { return (__atomic_fetch_or(&a[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST) & BIT_MASK(nr)) != 0; }
static inline int test_and_clear_bit(long nr, volatile unsigned long* a) { return (__atomic_fetch_and(&a[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_SEQ_CST) & BIT_MASK(nr)) != 0; }
#define __set_bit set_bit
#define __clear_bit clear_bit
#define __test_and_set_bit test_and_set_bit
#define __test_and_clear_bit test_and_clear_bit
#define clear_bit_unlock clear_bit
#define test_and_set_bit_lock test_and_set_bit
#define smp_mb__before_atomic() smp_mb()
#define smp_mb__after_atomic() smp_mb()

static inline unsigned long __ffs(unsigned long w) { return (unsigned long)__builtin_ctzl(w); }
static inline unsigned long __fls(unsigned long w) { return BITS_PER_LONG - 1 - (unsigned long)__builtin_clzl(w); }
static inline int ffs(int x) { return __builtin_ffs(x); }
static inline int fls(unsigned int x) { return x ? 32 - __builtin_clz(x) : 0; }
static inline int fls64(u64 x) { return x ? 64 - __builtin_clzll(x) : 0; }
static inline unsigned long ffz(unsigned long w) { return __ffs(~w); }
#define hweight8(w) __builtin_popcount((u8)(w))
#define hweight16(w) __builtin_popcount((u16)(w))
#define hweight32(w) __builtin_popcount((u32)(w))
#define hweight64(w) __builtin_popcountll((u64)(w))
#define hweight_long(w) __builtin_popcountl(w)
static inline int ilog2_lkpi(u64 n) { return fls64(n) - 1; }
#define ilog2(n) ilog2_lkpi(n)
#define is_power_of_2(n) ((n) != 0 && (((n) & ((n) - 1)) == 0))
static inline u64 roundup_pow_of_two(u64 n) { return n <= 1 ? 1 : 1ULL << fls64(n - 1); }
static inline u64 rounddown_pow_of_two(u64 n) { return 1ULL << (fls64(n) - 1); }
#define order_base_2(n) ((n) > 1 ? ilog2((n) - 1) + 1 : 0)
static inline u16 rol16(u16 w, unsigned int s) { return (u16)((w << (s & 15)) | (w >> ((-s) & 15))); }
static inline u16 ror16(u16 w, unsigned int s) { return (u16)((w >> (s & 15)) | (w << ((-s) & 15))); }
static inline u8 rol8(u8 w, unsigned int s) { return (u8)((w << (s & 7)) | (w >> ((-s) & 7))); }
static inline u8 ror8(u8 w, unsigned int s) { return (u8)((w >> (s & 7)) | (w << ((-s) & 7))); }
static inline u64 rol64(u64 w, unsigned int s) { return (w << (s & 63)) | (w >> ((-s) & 63)); }
static inline u64 ror64(u64 w, unsigned int s) { return (w >> (s & 63)) | (w << ((-s) & 63)); }
static inline unsigned int __ffs64(u64 w) { return (unsigned int)__builtin_ctzll(w); }
#define __roundup_pow_of_two(n) roundup_pow_of_two(n)
#define __rounddown_pow_of_two(n) rounddown_pow_of_two(n)
#define BITS_PER_TYPE(type) (sizeof(type) * BITS_PER_BYTE)
#define BITS_TO_U32(nr) DIV_ROUND_UP(nr, 32)
#define BITS_TO_BYTES(nr) DIV_ROUND_UP(nr, 8)
#define sign_extend32(value, index) ((s32)((u32)(value) << (31 - (index))) >> (31 - (index)))
#define sign_extend64(value, index) ((s64)((u64)(value) << (63 - (index))) >> (63 - (index)))
#define for_each_clear_bit(bit, addr, size) \
    for ((bit) = find_first_zero_bit((addr), (size)); (bit) < (size); (bit) = find_next_zero_bit((addr), (size), (bit) + 1))
#define assign_bit(nr, addr, value) ((value) ? set_bit((nr), (addr)) : clear_bit((nr), (addr)))
#define __assign_bit assign_bit
static inline int test_and_change_bit(long nr, volatile unsigned long* a) { return (__atomic_fetch_xor(&a[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_SEQ_CST) & BIT_MASK(nr)) != 0; }
static inline unsigned long find_last_bit(const unsigned long* addr, unsigned long size) {
    while (size--) if (test_bit(size, addr)) return size;
    return ~0UL;
}
static inline void bitmap_set(unsigned long* map, unsigned int start, unsigned int len) { while (len--) set_bit(start++, map); }
static inline void bitmap_clear(unsigned long* map, unsigned int start, unsigned int len) { while (len--) clear_bit(start++, map); }
static inline int bitmap_weight(const unsigned long* src, unsigned int nbits) {
    int w = 0; for (unsigned int i = 0; i < nbits; i++) w += test_bit(i, src); return w;
}
static inline int bitmap_equal(const unsigned long* a, const unsigned long* b, unsigned int nbits) {
    for (unsigned int i = 0; i < nbits; i++) if (test_bit(i, a) != test_bit(i, b)) return 0;
    return 1;
}
static inline int bitmap_and(unsigned long* dst, const unsigned long* a, const unsigned long* b, unsigned int nbits) {
    unsigned long r = 0; for (unsigned int k = 0; k < BITS_TO_LONGS(nbits); k++) r |= (dst[k] = a[k] & b[k]); return r != 0;
}
static inline void bitmap_or(unsigned long* dst, const unsigned long* a, const unsigned long* b, unsigned int nbits) {
    for (unsigned int k = 0; k < BITS_TO_LONGS(nbits); k++) dst[k] = a[k] | b[k];
}
static inline int bitmap_intersects(const unsigned long* a, const unsigned long* b, unsigned int nbits) {
    for (unsigned int i = 0; i < nbits; i++) if (test_bit(i, a) && test_bit(i, b)) return 1;
    return 0;
}
static inline int bitmap_subset(const unsigned long* a, const unsigned long* b, unsigned int nbits) {
    for (unsigned int i = 0; i < nbits; i++) if (test_bit(i, a) && !test_bit(i, b)) return 0;
    return 1;
}
#define bitmap_full(src, nbits) (find_first_zero_bit(src, nbits) >= (nbits))
#define bitmap_from_arr32(bitmap, buf, nbits) memcpy(bitmap, buf, BITS_TO_BYTES(nbits))
#define bitmap_to_arr32(buf, bitmap, nbits) memcpy(buf, bitmap, BITS_TO_BYTES(nbits))
unsigned long* bitmap_zalloc(unsigned int nbits, gfp_t flags);
unsigned long* bitmap_alloc(unsigned int nbits, gfp_t flags);
void bitmap_free(const unsigned long* bitmap);
static inline u32 rol32(u32 w, unsigned int s) { return (w << (s & 31)) | (w >> ((-s) & 31)); }
static inline u32 ror32(u32 w, unsigned int s) { return (w >> (s & 31)) | (w << ((-s) & 31)); }

unsigned long find_first_bit(const unsigned long* addr, unsigned long size);
unsigned long find_first_zero_bit(const unsigned long* addr, unsigned long size);
unsigned long find_next_bit(const unsigned long* addr, unsigned long size, unsigned long offset);
unsigned long find_next_zero_bit(const unsigned long* addr, unsigned long size, unsigned long offset);
#define for_each_set_bit(bit, addr, size) \
    for ((bit) = find_first_bit((addr), (size)); (bit) < (size); (bit) = find_next_bit((addr), (size), (bit) + 1))
static inline void bitmap_zero(unsigned long* dst, unsigned int nbits) { memset(dst, 0, BITS_TO_LONGS(nbits) * sizeof(long)); }
static inline void bitmap_fill(unsigned long* dst, unsigned int nbits) { memset(dst, 0xff, BITS_TO_LONGS(nbits) * sizeof(long)); }
static inline int bitmap_empty(const unsigned long* src, unsigned int nbits) { return find_first_bit(src, nbits) >= nbits; }
static inline void bitmap_copy(unsigned long* dst, const unsigned long* src, unsigned int nbits) { memcpy(dst, src, BITS_TO_LONGS(nbits) * sizeof(long)); }

LKPI_EXTERN_C_END

#ifndef __cplusplus
#include <linux/bitfield.h>
#endif

#endif
