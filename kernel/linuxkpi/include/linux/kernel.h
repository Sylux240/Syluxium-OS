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
#ifdef DEBUG
#define pr_debug(fmt, ...) printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__)
#else /* as in Linux without dynamic debug: compiled out (arguments still checked) */
#define pr_debug(fmt, ...) ({ if (0) printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__); 0; })
#endif
#define lkpi_print_once(fn, ...) ({ static bool __printed; if (!__printed) { __printed = true; fn(__VA_ARGS__); } })
#define pr_err_once(...) lkpi_print_once(pr_err, __VA_ARGS__)
#define pr_warn_once(...) lkpi_print_once(pr_warn, __VA_ARGS__)
#define pr_info_once(...) lkpi_print_once(pr_info, __VA_ARGS__)
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
#define WARN_ON_ONCE(c) ({ static bool __warned; int __w = !!(c); if (unlikely(__w) && !__warned) { __warned = true; lkpi_warn(__FILE__, __LINE__, #c); } unlikely(__w); })
#define WARN(c, fmt, ...) ({ int __w = !!(c); if (unlikely(__w)) { printk(KERN_WARNING fmt, ##__VA_ARGS__); lkpi_warn(__FILE__, __LINE__, #c); } unlikely(__w); })
#define WARN_ONCE(c, fmt, ...) WARN(c, fmt, ##__VA_ARGS__)
#ifdef __cplusplus
#define BUILD_BUG_ON(c) static_assert(!(c), "BUILD_BUG_ON: " #c)
#else
#define BUILD_BUG_ON(c) BUILD_BUG_ON_MSG(c, "BUILD_BUG_ON failed: " #c)
#endif
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
#define DIV_ROUND_UP_ULL(ll, d) ((unsigned long long)(((unsigned long long)(ll) + (d) - 1) / (d)))
#define DIV_ROUND_DOWN_ULL(ll, d) ((unsigned long long)(ll) / (d))
#define DIV_ROUND_CLOSEST_ULL(x, d) (((unsigned long long)(x) + ((d) / 2)) / (d))
#define __round_mask(x, y) ((__typeof__(x))((y) - 1))
#define round_up(x, y) ((((x) - 1) | __round_mask(x, y)) + 1)
#define round_down(x, y) ((x) & ~__round_mask(x, y))
#define do_div(n, base) ({ u32 __base = (base); u32 __rem = (u32)((u64)(n) % __base); (n) = (u64)(n) / __base; __rem; })
#define mult_frac(x, n, d) ({ __typeof__(x) __q = (x) / (d); __typeof__(x) __r = (x) % (d); (__q * (n)) + ((__r * (n)) / (d)); })
#define min3(x, y, z) min((__typeof__(x))min(x, y), z)
#define max3(x, y, z) max((__typeof__(x))max(x, y), z)
#define min_not_zero(x, y) ({ __typeof__(x) __x = (x); __typeof__(y) __y = (y); __x == 0 ? __y : ((__y == 0) ? __x : min(__x, __y)); })
#define umin(x, y) min_t(unsigned long long, x, y)
#define umax(x, y) max_t(unsigned long long, x, y)
#define in_range(val, start, len) ((val) >= (start) && (val) < (start) + (len))
#define abs_diff(a, b) ({ __typeof__(a) __a = (a); __typeof__(b) __b = (b); __a > __b ? (__a - __b) : (__b - __a); })
#define swap_lkpi swap
#define struct_size(p, member, count) (sizeof(*(p)) + sizeof(*(p)->member) * (size_t)(count))
#define struct_size_t(type, member, count) (sizeof(type) + sizeof(((type*)0)->member[0]) * (size_t)(count))
#define flex_array_size(p, member, count) (sizeof(*(p)->member) * (size_t)(count))
#define array_size(a, b) ((size_t)(a) * (size_t)(b))
#define array3_size(a, b, c) ((size_t)(a) * (size_t)(b) * (size_t)(c))
#define size_add(a, b) ((size_t)(a) + (size_t)(b))
#define size_mul(a, b) ((size_t)(a) * (size_t)(b))
#define check_add_overflow(a, b, d) __builtin_add_overflow(a, b, d)
#define check_sub_overflow(a, b, d) __builtin_sub_overflow(a, b, d)
#define check_mul_overflow(a, b, d) __builtin_mul_overflow(a, b, d)
#define IS_ALIGNED_LKPI IS_ALIGNED
#define FIELD_SIZEOF(t, f) (sizeof(((t*)0)->f))
#define REPEAT_BYTE(x) ((~0ul / 0xff) * (x))
#define hex_asc_lo(x) hex_asc[((x) & 0x0f)]
#define hex_asc_hi(x) hex_asc[((x) & 0xf0) >> 4]
extern const char hex_asc[];
int hex_to_bin(unsigned char ch);
int hex2bin(u8* dst, const char* src, size_t count);
char* bin2hex(char* dst, const void* src, size_t count);
u64 lkpi_div_u64(u64 a, u32 b);
#define div_u64(a, b) ((u64)(a) / (u32)(b))
#define div_s64(a, b) ((s64)(a) / (s32)(b))
#define div64_u64(a, b) ((u64)(a) / (u64)(b))
#define div64_s64(a, b) ((s64)(a) / (s64)(b))
#define div64_ul(a, b) ((u64)(a) / (unsigned long)(b))
#define div_u64_rem(a, b, r) ({ u64 __a = (a); u32 __b = (b); *(r) = (u32)(__a % __b); __a / __b; })
#define mul_u32_u32(a, b) ((u64)(a) * (b))
#define int_sqrt(x) lkpi_int_sqrt(x)
unsigned long lkpi_int_sqrt(unsigned long x);
#define reciprocal_scale(val, ep_ro) ((u32)(((u64)(val) * (ep_ro)) >> 32))
#define upper_16_bits(n) ((u16)((n) >> 16))
#define lower_16_bits(n) ((u16)((n) & 0xffff))
#define might_sleep_if(c) do { } while (0)
#define might_fault() do { } while (0)
#define cant_sleep() do { } while (0)
#define lockdep_assert_not_held(l) do { (void)(l); } while (0)
#define lockdep_assert(c) do { } while (0)
#define lockdep_is_held(l) 1
#define lockdep_assert_held_once(l) do { (void)(l); } while (0)
#define lockdep_assert_wiphy(w) do { (void)(w); } while (0)
#define lockdep_assert_in_softirq() do { } while (0)
#define lockdep_set_class_and_name(l, k, n) do { } while (0)
#define lockdep_register_key(k) do { } while (0)
#define lockdep_unregister_key(k) do { } while (0)
struct lock_class_key { int unused; };
#define WARN_ON_SMP(c) WARN_ON(c)
#define WARN_RATELIMIT(c, fmt, ...) WARN(c, fmt, ##__VA_ARGS__)
#define BUG_ON_ONCE BUG_ON
#define pr_devel(fmt, ...) do { if (0) printk(fmt, ##__VA_ARGS__); } while (0)
#define no_printk(fmt, ...) ({ if (0) printk(fmt, ##__VA_ARGS__); 0; })
#define print_hex_dump(...) do { } while (0)
#define print_hex_dump_bytes(...) do { } while (0)
#define print_hex_dump_debug(...) do { } while (0)
#define printk_ratelimited printk
#define printk_once(...) lkpi_print_once(printk, __VA_ARGS__)
#define pr_debug_once pr_debug
#define pr_notice_once(...) lkpi_print_once(pr_notice, __VA_ARGS__)
#define pr_warn_ratelimited_once pr_warn
#define dump_stack() do { } while (0)
#ifndef __cplusplus
#define panic(fmt, ...) lkpi_bug(__FILE__, __LINE__)
#endif
#define kstrtou8(s, b, r) lkpi_kstrtou8(s, b, r)
int lkpi_kstrtou8(const char* s, unsigned int base, u8* res);
int kstrtou16(const char* s, unsigned int base, u16* res);
int kstrtou32(const char* s, unsigned int base, u32* res);
int kstrtos32(const char* s, unsigned int base, s32* res);
int kstrtobool(const char* s, bool* res);
int kstrtoull(const char* s, unsigned int base, unsigned long long* res);
int kstrtoll(const char* s, unsigned int base, long long* res);
int kstrtol(const char* s, unsigned int base, long* res);
int sscanf(const char* buf, const char* fmt, ...);
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
#define S8_MAX ((s8)(U8_MAX >> 1))
#define S8_MIN ((s8)(-S8_MAX - 1))
#define S16_MAX ((s16)(U16_MAX >> 1))
#define S16_MIN ((s16)(-S16_MAX - 1))
#define S32_MIN ((s32)(-S32_MAX - 1))
#define S64_MAX ((s64)(U64_MAX >> 1))
#define S64_MIN ((s64)(-S64_MAX - 1))
#define INT_MIN (-INT_MAX - 1)
#define LONG_MIN (-LONG_MAX - 1)
#define SIZE_MAX (~(size_t)0)
#define USHRT_MAX ((unsigned short)~0U)
#define SHRT_MAX ((short)(USHRT_MAX >> 1))
#define UCHAR_MAX ((unsigned char)~0U)
#define LLONG_MAX ((long long)(~0ULL >> 1))
#define ULLONG_MAX (~0ULL)
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
#define be16_to_cpup(p) be16_to_cpu(*(p))
#define be32_to_cpup(p) be32_to_cpu(*(p))
#define be64_to_cpup(p) be64_to_cpu(*(p))
#define le64_to_cpup(p) le64_to_cpu(*(p))
#define cpu_to_le32p(p) cpu_to_le32(*(p))
#define cpu_to_be16p(p) cpu_to_be16(*(p))
#define le32_add_cpu(p, v) (*(p) = cpu_to_le32(le32_to_cpu(*(p)) + (v)))
#define be16_add_cpu(p, v) (*(p) = cpu_to_be16(be16_to_cpu(*(p)) + (v)))
#define cpu_to_le32s(p) do { } while (0)
#define le32_to_cpus(p) do { } while (0)
#define le16_to_cpus(p) do { } while (0)
#define __cpu_to_le16(x) cpu_to_le16(x)
#define __cpu_to_le32(x) cpu_to_le32(x)
#define __cpu_to_le64(x) cpu_to_le64(x)
#define __le16_to_cpu(x) le16_to_cpu(x)
#define __le32_to_cpu(x) le32_to_cpu(x)
#define __le64_to_cpu(x) le64_to_cpu(x)
#define __cpu_to_be16(x) cpu_to_be16(x)
#define __cpu_to_be32(x) cpu_to_be32(x)
#define __cpu_to_be64(x) cpu_to_be64(x)
#define __be16_to_cpu(x) be16_to_cpu(x)
#define __be32_to_cpu(x) be32_to_cpu(x)
#define __be64_to_cpu(x) be64_to_cpu(x)
#define __constant_htons(x) ((__be16)__builtin_bswap16(x))
#define __constant_cpu_to_le16(x) cpu_to_le16(x)
#define __constant_cpu_to_le32(x) cpu_to_le32(x)
#define __constant_cpu_to_be16(x) cpu_to_be16(x)
#define __constant_cpu_to_be32(x) cpu_to_be32(x)
#define __swab16(x) __builtin_bswap16(x)
#define __swab32(x) __builtin_bswap32(x)
#define __swab64(x) __builtin_bswap64(x)
#define swab64(x) __builtin_bswap64(x)
#define __LITTLE_ENDIAN 1234
#define __LITTLE_ENDIAN_BITFIELD 1
#define ntohs(x) be16_to_cpu(x)
#define htonl(x) cpu_to_be32(x)
#define ntohl(x) be32_to_cpu(x)

#ifndef __cplusplus
LKPI_EXTERN_C_END
#include <linux/unaligned.h>
LKPI_EXTERN_C_BEGIN
#else
static inline u16 get_unaligned_le16(const void* p) { u16 v; __builtin_memcpy(&v, p, 2); return v; }
static inline u32 get_unaligned_le32(const void* p) { u32 v; __builtin_memcpy(&v, p, 4); return v; }
static inline u64 get_unaligned_le64(const void* p) { u64 v; __builtin_memcpy(&v, p, 8); return v; }
static inline void put_unaligned_le16(u16 v, void* p) { __builtin_memcpy(p, &v, 2); }
static inline void put_unaligned_le32(u32 v, void* p) { __builtin_memcpy(p, &v, 4); }
static inline u16 get_unaligned_be16(const void* p) { return be16_to_cpu(get_unaligned_le16(p)); }
static inline u32 get_unaligned_be32(const void* p) { return be32_to_cpu(get_unaligned_le32(p)); }
#endif

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

#define array_index_nospec(index, size) ({ __typeof__(index) __i = (index); __i < (size) ? __i : 0; })
#define barrier_nospec() do { } while (0)

/* The running thread, as far as Linux code looks at it. */
struct task_struct {
    char comm[16];
    int pid;
    int tgid;
    volatile long __state; /* TASK_RUNNING / TASK_INTERRUPTIBLE / TASK_UNINTERRUPTIBLE */
    void* lkpi_thread;     /* the Syluxium thread */
    struct mm_struct* mm;  /* appended: always NULL (no Linux address spaces) */
    unsigned int flags;    /* PF_* (none set) */
    int exit_code;
    struct task_struct* group_leader; /* the process's task (binder: one per process) */
    int lkpi_usage;        /* get/put_task_struct; a process's task is freed at 0 */
    void* lkpi_process;    /* a process's task: its proc::Process (NULL once it's gone) */
};
struct task_struct* lkpi_current(void);

/* Linux's open-coded sleep: set_current_state(); check; schedule_timeout().
 * A wake-up between the two isn't lost: it resets the state to running. */
#define TASK_RUNNING 0x0000
#define TASK_INTERRUPTIBLE 0x0001
#define TASK_UNINTERRUPTIBLE 0x0002
#define TASK_KILLABLE (TASK_UNINTERRUPTIBLE | 0x0100)
#define TASK_IDLE (TASK_UNINTERRUPTIBLE | 0x0400)
#define TASK_NORMAL (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)
void lkpi_set_current_state(long state);
#define set_current_state(s) lkpi_set_current_state(s)
#define __set_current_state(s) lkpi_set_current_state(s)
int wake_up_state(struct task_struct* p, unsigned int state);
int wake_up_process(struct task_struct* p);
int lkpi_signal_pending(void);
#define signal_pending(t) ((t) == lkpi_current() ? lkpi_signal_pending() : 0)
#define fatal_signal_pending(t) signal_pending(t)
#define signal_pending_state(state, t) (((state) & TASK_INTERRUPTIBLE) && signal_pending(t))
#ifndef __cplusplus
#define current lkpi_current()
#define in_interrupt() 0
#define softirq_count() (1 << 8) /* callers may assume BH-disabled context: LinuxKPI code never runs in a hard IRQ */
#define in_serving_softirq() 0
#define hardirq_count() 0
#define in_hardirq() 0
#define in_nmi() 0
#define in_softirq() 0
#define in_task() 1
#define in_atomic() 0
#define irqs_disabled() 0
#endif

LKPI_EXTERN_C_END

#include <linux/string.h>

#endif
