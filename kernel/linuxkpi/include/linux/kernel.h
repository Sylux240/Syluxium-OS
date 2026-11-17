/* LinuxKPI core: printing, errors, helpers. Most other linux/ headers
 * include this one. */
#ifndef _LKPI_LINUX_KERNEL_H
#define _LKPI_LINUX_KERNEL_H

#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/types.h>

LKPI_EXTERN_C_BEGIN

/* ---- printing ---- */
#define KERN_SOH "\001"
#define KERN_EMERG KERN_SOH "0"
#define KERN_ALERT KERN_SOH "1"
#define KERN_CRIT KERN_SOH "2"
#define KERN_ERR KERN_SOH "3"
#define KERN_WARNING KERN_SOH "4"
#define KERN_NOTICE KERN_SOH "5"
#define KERN_INFO KERN_SOH "6"
#define KERN_DEBUG KERN_SOH "7"
#define KERN_CONT KERN_SOH "c"

/* The kernel's own (C++) formatting functions share these names. */
#define snprintf lkpi_snprintf
#define vsnprintf lkpi_vsnprintf
#define scnprintf lkpi_scnprintf
#define sprintf lkpi_sprintf

int printk(const char* fmt, ...) __printf(1, 2);
int vprintk(const char* fmt, va_list args);
int snprintf(char* buf, size_t size, const char* fmt, ...) __printf(3, 4);
int vsnprintf(char* buf, size_t size, const char* fmt, va_list args);
int scnprintf(char* buf, size_t size, const char* fmt, ...) __printf(3, 4);
int sprintf(char* buf, const char* fmt, ...) __printf(2, 3);
char* kasprintf(gfp_t gfp, const char* fmt, ...) __printf(2, 3);

#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif
#define pr_emerg(fmt, ...) printk(KERN_EMERG pr_fmt(fmt), ##__VA_ARGS__)
#define pr_alert(fmt, ...) printk(KERN_ALERT pr_fmt(fmt), ##__VA_ARGS__)
#define pr_crit(fmt, ...) printk(KERN_CRIT pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err(fmt, ...) printk(KERN_ERR pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warn(fmt, ...) printk(KERN_WARNING pr_fmt(fmt), ##__VA_ARGS__)
#define pr_warning pr_warn
#define pr_notice(fmt, ...) printk(KERN_NOTICE pr_fmt(fmt), ##__VA_ARGS__)
#define pr_info(fmt, ...) printk(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#define pr_cont(fmt, ...) printk(KERN_CONT fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...) printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)
#define pr_err_once pr_err
#define pr_warn_once pr_warn
#define pr_info_once pr_info
#define pr_err_ratelimited pr_err
#define pr_warn_ratelimited pr_warn
#define pr_info_ratelimited pr_info
#define pr_debug_ratelimited pr_debug

/* ---- assertions ---- */
void lkpi_bug(const char* file, int line) __attribute__((noreturn));
void lkpi_warn(const char* file, int line, const char* cond);
#define BUG() lkpi_bug(__FILE__, __LINE__)
#define BUG_ON(c) do { if (unlikely(c)) BUG(); } while (0)
#define WARN_ON(c) ({ int __w = !!(c); if (unlikely(__w)) lkpi_warn(__FILE__, __LINE__, #c); unlikely(__w); })
#define WARN_ON_ONCE(c) WARN_ON(c)
#define WARN(c, fmt, ...) ({ int __w = !!(c); if (unlikely(__w)) { printk(KERN_WARNING fmt, ##__VA_ARGS__); lkpi_warn(__FILE__, __LINE__, #c); } unlikely(__w); })
#define WARN_ONCE(c, fmt, ...) WARN(c, fmt, ##__VA_ARGS__)
#define BUILD_BUG_ON(c) _Static_assert(!(c), "BUILD_BUG_ON: " #c)
#define might_sleep() do { } while (0)
#define lockdep_assert_held(l) do { (void)(l); } while (0)

/* ---- helpers ---- */
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define container_of(ptr, type, member) ((type*)((char*)(ptr) - offsetof(type, member)))
#define min(a, b) ({ __typeof__(a) __a = (a); __typeof__(b) __b = (b); __a < __b ? __a : __b; })
#define max(a, b) ({ __typeof__(a) __a = (a); __typeof__(b) __b = (b); __a > __b ? __a : __b; })
#define min_t(t, a, b) ({ t __a = (a); t __b = (b); __a < __b ? __a : __b; })
#define max_t(t, a, b) ({ t __a = (a); t __b = (b); __a > __b ? __a : __b; })
#define clamp(v, lo, hi) min(max(v, lo), hi)
#define clamp_t(t, v, lo, hi) min_t(t, max_t(t, v, lo), hi)
#define clamp_val(v, lo, hi) clamp_t(__typeof__(v), v, lo, hi)
#define swap(a, b) do { __typeof__(a) __t = (a); (a) = (b); (b) = __t; } while (0)
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define DIV_ROUND_CLOSEST(x, d) (((x) + ((d) / 2)) / (d))
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#define rounddown(x, y) ((x) - ((x) % (y)))
#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((__typeof__(x))(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((__typeof__(x))(a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((__typeof__(x))(a) - 1)) == 0)
#define PTR_ALIGN(p, a) ((__typeof__(p))ALIGN((unsigned long)(p), (a)))
#define upper_32_bits(n) ((u32)(((u64)(n)) >> 32))
#define lower_32_bits(n) ((u32)((n) & 0xffffffff))
#define abs(x) ({ __typeof__(x) __x = (x); __x < 0 ? -__x : __x; })
#define U8_MAX ((u8)~0U)
#define U16_MAX ((u16)~0U)
#define U32_MAX ((u32)~0U)
#define U64_MAX ((u64)~0ULL)
#define S32_MAX ((s32)(U32_MAX >> 1))
#define INT_MAX ((int)(~0U >> 1))
#define UINT_MAX (~0U)
#define LONG_MAX ((long)(~0UL >> 1))
#define ULONG_MAX (~0UL)
#define PAGE_SIZE 4096UL
#define PAGE_SHIFT 12
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define HZ 1000
#define BITS_PER_LONG 64
#define BITS_PER_BYTE 8

/* Byte order (x86 is little-endian). */
#define cpu_to_le16(x) ((__le16)(u16)(x))
#define cpu_to_le32(x) ((__le32)(u32)(x))
#define cpu_to_le64(x) ((__le64)(u64)(x))
#define le16_to_cpu(x) ((u16)(__le16)(x))
#define le32_to_cpu(x) ((u32)(__le32)(x))
#define le64_to_cpu(x) ((u64)(__le64)(x))
#define cpu_to_be16(x) ((__be16)__builtin_bswap16(x))
#define cpu_to_be32(x) ((__be32)__builtin_bswap32(x))
#define cpu_to_be64(x) ((__be64)__builtin_bswap64(x))
#define be16_to_cpu(x) __builtin_bswap16((u16)(x))
#define be32_to_cpu(x) __builtin_bswap32((u32)(x))
#define be64_to_cpu(x) __builtin_bswap64((u64)(x))
#define le16_to_cpup(p) le16_to_cpu(*(p))
#define le32_to_cpup(p) le32_to_cpu(*(p))
#define cpu_to_le16s(p) do { } while (0)
#define le16_add_cpu(p, v) (*(p) = cpu_to_le16(le16_to_cpu(*(p)) + (v)))
#define htons(x) cpu_to_be16(x)
#define ntohs(x) be16_to_cpu(x)
#define htonl(x) cpu_to_be32(x)
#define ntohl(x) be32_to_cpu(x)

static inline u16 get_unaligned_le16(const void* p) { u16 v; __builtin_memcpy(&v, p, 2); return v; }
static inline u32 get_unaligned_le32(const void* p) { u32 v; __builtin_memcpy(&v, p, 4); return v; }
static inline u64 get_unaligned_le64(const void* p) { u64 v; __builtin_memcpy(&v, p, 8); return v; }
static inline void put_unaligned_le16(u16 v, void* p) { __builtin_memcpy(p, &v, 2); }
static inline void put_unaligned_le32(u32 v, void* p) { __builtin_memcpy(p, &v, 4); }
static inline u16 get_unaligned_be16(const void* p) { return be16_to_cpu(get_unaligned_le16(p)); }
static inline u32 get_unaligned_be32(const void* p) { return be32_to_cpu(get_unaligned_le32(p)); }

/* String/number parsing used by drivers. */
int kstrtoint(const char* s, unsigned int base, int* res);
int kstrtouint(const char* s, unsigned int base, unsigned int* res);
int kstrtoul(const char* s, unsigned int base, unsigned long* res);
unsigned long simple_strtoul(const char* s, char** end, unsigned int base);

/* Error pointers (linux/err.h). */
#define MAX_ERRNO 4095
#define IS_ERR_VALUE(x) unlikely((unsigned long)(void*)(x) >= (unsigned long)-MAX_ERRNO)
static inline void* ERR_PTR(long error) { return (void*)error; }
static inline long PTR_ERR(const void* ptr) { return (long)ptr; }
static inline bool IS_ERR(const void* ptr) { return IS_ERR_VALUE((unsigned long)ptr); }
static inline bool IS_ERR_OR_NULL(const void* ptr) { return !ptr || IS_ERR_VALUE((unsigned long)ptr); }
static inline void* ERR_CAST(const void* ptr) { return (void*)ptr; }
static inline int PTR_ERR_OR_ZERO(const void* ptr) { return IS_ERR(ptr) ? (int)PTR_ERR(ptr) : 0; }

LKPI_EXTERN_C_END

#include <linux/string.h>

#endif
