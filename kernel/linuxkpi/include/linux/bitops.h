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
#define __bf_shf(x) (__builtin_ffsll(x) - 1)
#define FIELD_PREP(mask, val) (((__typeof__(mask))(val) << __bf_shf(mask)) & (mask))
#define FIELD_GET(mask, reg) ((__typeof__(mask))(((reg) & (mask)) >> __bf_shf(mask)))
#define FIELD_MAX(mask) ((__typeof__(mask))((mask) >> __bf_shf(mask)))
#define u32_get_bits(v, m) FIELD_GET(m, v)
#define u32_encode_bits(v, m) FIELD_PREP(m, v)
#define le32_get_bits(v, m) FIELD_GET(m, le32_to_cpu(v))
#define le32_encode_bits(v, m) cpu_to_le32(FIELD_PREP(m, v))

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

#endif
