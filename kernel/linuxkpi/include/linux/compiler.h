#ifndef _LKPI_LINUX_COMPILER_H
#define _LKPI_LINUX_COMPILER_H

#include <linux/types.h>

#ifdef __cplusplus
#define LKPI_EXTERN_C_BEGIN extern "C" {
#define LKPI_EXTERN_C_END }
#else
#define LKPI_EXTERN_C_BEGIN
#define LKPI_EXTERN_C_END
#endif

#define __iomem
#define __user
#define __force
#define __rcu
#define __percpu
#define __must_check __attribute__((warn_unused_result))
#define __always_inline inline __attribute__((always_inline))
#define noinline __attribute__((noinline))
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __maybe_unused __attribute__((unused))
#define __always_unused __attribute__((unused))
#define __used __attribute__((used))
#define __cold __attribute__((cold))
#define __printf(a, b) __attribute__((format(printf, a, b)))
#define __init
#define __exit
#define __initdata
#define __devinit
#define __read_mostly
#define __cacheline_aligned __attribute__((aligned(64)))
#define ____cacheline_aligned __attribute__((aligned(64)))
#define fallthrough __attribute__((fallthrough))
#define __counted_by(x)

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif
#define barrier() __asm__ __volatile__("" ::: "memory")
#define READ_ONCE(x) (*(const volatile __typeof__(x)*)&(x))
#define WRITE_ONCE(x, v) (*(volatile __typeof__(x)*)&(x) = (v))

/* As on Linux/x86: mb/rmb/wmb are real fences (mfence/lfence/sfence), since
 * drivers order stores to write-combining memory (GPU rings, page tables
 * in VRAM through the BAR, GTT pages mapped WC) against doorbells and
 * DMA with them; a compiler-only fence leaves WC stores in the CPU's
 * write-combining buffers. smp_* and dma_* order normal (write-back)
 * memory, where x86's TSO only needs the compiler barrier, except smp_mb. */
#define mb() __asm__ __volatile__("mfence" ::: "memory")
#define rmb() __asm__ __volatile__("lfence" ::: "memory")
#define wmb() __asm__ __volatile__("sfence" ::: "memory")
#define smp_mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define smp_rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define smp_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#define dma_rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define dma_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#define smp_store_release(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)
#define smp_load_acquire(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)

#include <linux/compiler_ext.h>

#endif
#define __deprecated
