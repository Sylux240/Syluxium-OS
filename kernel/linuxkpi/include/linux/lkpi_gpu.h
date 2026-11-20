/* LinuxKPI: kernel interfaces the GPU drivers (TTM, i915, amdgpu, radeon,
 * nouveau, bochs) expect from the common headers. Included by lkpi_all.h. */
#ifndef _LKPI_LINUX_GPU_H
#define _LKPI_LINUX_GPU_H

#include <linux/lkpi_page.h>

LKPI_EXTERN_C_BEGIN

/* ---- pages & memory ---- */
#ifndef PFN_UP
#define PFN_UP(x) (((x) + PAGE_SIZE - 1) >> PAGE_SHIFT)
#define PFN_DOWN(x) ((x) >> PAGE_SHIFT)
#endif
#define NUMA_NO_NODE (-1)
#define numa_node_id() 0
#define cpu_to_node(cpu) 0
#define page_to_nid(p) 0
#ifndef __GFP_THISNODE
#define __GFP_THISNODE 0
#endif
#ifndef __GFP_NOMEMALLOC
#define __GFP_NOMEMALLOC 0
#endif
#define want_init_on_free() false
#define want_init_on_alloc(gfp) false
static inline void clear_page(void *page) { __builtin_memset(page, 0, PAGE_SIZE); }
static inline void copy_page(void *to, void *from) { __builtin_memcpy(to, from, PAGE_SIZE); }
#define clear_highpage(p) clear_page(page_address(p))
#define copy_highpage(to, from) copy_page(page_address(to), page_address(from))
#define kmap_local_page_prot(p, prot) lkpi_page_address(p)
#define kmap_atomic_prot(p, prot) lkpi_page_address(p)
struct page *vmalloc_to_page(const void *addr);
unsigned long vmalloc_to_pfn(const void *addr);
bool is_vmalloc_addr(const void *addr);

struct sysinfo {
	long uptime;
	unsigned long loads[3];
	unsigned long totalram, freeram, sharedram, bufferram, totalswap, freeswap;
	unsigned short procs, pad;
	unsigned long totalhigh, freehigh;
	unsigned int mem_unit;
};
void si_meminfo(struct sysinfo *val);
unsigned long lkpi_totalram_pages(void);
#define totalram_pages() lkpi_totalram_pages()
#define si_mem_available() lkpi_totalram_pages()

/* ---- atomics ---- */
#ifndef atomic_long_add
#define atomic_long_add atomic_add
#define atomic_long_sub atomic_sub
#define atomic_long_add_return atomic_add_return
#define atomic_long_sub_return atomic_sub_return
#define atomic_long_inc_return atomic_inc_return
#define atomic_long_xchg atomic_xchg
#endif

/* ---- memory shrinkers: registered, never called (no memory pressure
 * reclaim here; GPU drivers keep working with what they have) ---- */
#define SHRINK_STOP (~0UL)
#define SHRINK_EMPTY (~0UL - 1)
#define DEFAULT_SEEKS 2
struct shrink_control {
	gfp_t gfp_mask;
	int nid;
	unsigned long nr_to_scan;
	unsigned long nr_scanned;
	void *memcg;
};
struct shrinker {
	unsigned long (*count_objects)(struct shrinker *, struct shrink_control *sc);
	unsigned long (*scan_objects)(struct shrinker *, struct shrink_control *sc);
	long batch;
	int seeks;
	unsigned flags;
	void *private_data;
};
struct shrinker *shrinker_alloc(unsigned int flags, const char *fmt, ...);
#define shrinker_register(s) do { (void)(s); } while (0)
void shrinker_free(struct shrinker *s);
#define fs_reclaim_acquire(gfp) do { } while (0)
#define fs_reclaim_release(gfp) do { } while (0)
#define register_oom_notifier(nb) 0
#define unregister_oom_notifier(nb) 0
#define current_is_kswapd() false

/* ---- DMA attributes (no IOMMU: bus address = physical) ---- */
#define DMA_ATTR_WEAK_ORDERING (1UL << 1)
#define DMA_ATTR_WRITE_COMBINE (1UL << 2)
#define DMA_ATTR_NO_KERNEL_MAPPING (1UL << 4)
#define DMA_ATTR_SKIP_CPU_SYNC (1UL << 5)
#define DMA_ATTR_FORCE_CONTIGUOUS (1UL << 6)
#define DMA_ATTR_ALLOC_SINGLE_PAGES (1UL << 7)
#define DMA_ATTR_NO_WARN (1UL << 8)
#define DMA_ATTR_PRIVILEGED (1UL << 9)
#define dma_alloc_attrs(dev, size, handle, gfp, attrs) dma_alloc_coherent(dev, size, handle, gfp)
#define dma_free_attrs(dev, size, cpu, handle, attrs) dma_free_coherent(dev, size, cpu, handle)

/* ---- workqueues ---- */
#define queue_work_node(node, wq, work) queue_work(wq, work)

/* ---- page faults on driver mappings ---- */
#define FAULT_FLAG_WRITE 0x01
#define FAULT_FLAG_MKWRITE 0x02
#define FAULT_FLAG_ALLOW_RETRY 0x04
#define FAULT_FLAG_RETRY_NOWAIT 0x08
#define FAULT_FLAG_KILLABLE 0x10
#define FAULT_FLAG_TRIED 0x20
#define FAULT_FLAG_USER 0x40
#define FAULT_FLAG_INTERRUPTIBLE 0x200
#define fault_flag_allow_retry_first(flags) false
struct mm_struct;
#define mmap_read_lock(mm) do { (void)(mm); } while (0)
#define mmap_read_unlock(mm) do { (void)(mm); } while (0)
#define mmap_read_trylock(mm) true
#define mmap_write_lock(mm) do { (void)(mm); } while (0)
#define mmap_write_unlock(mm) do { (void)(mm); } while (0)
#define mmap_assert_locked(mm) do { } while (0)
#define mmap_assert_write_locked(mm) do { } while (0)

/* ---- misc kernel core ---- */
typedef __UINTPTR_TYPE__ uintptr_t;
typedef int pci_power_t;
#ifndef MAX_JIFFY_OFFSET
#define MAX_JIFFY_OFFSET ((LONG_MAX >> 1) - 1)
#endif
#define LOCKDEP_STILL_OK 0
#define LOCKDEP_NOW_UNRELIABLE 1
#define TAINT_WARN 9
#define TAINT_USER 6
#define TAINT_DIE 7
#define TAINT_AUX 16
#define add_taint(flag, lockdep_ok) do { } while (0)
#define test_taint(flag) 0
#define atomic_set_release(v, i) __atomic_store_n(&(v)->counter, (i), __ATOMIC_RELEASE)
#define atomic_read_acquire(v) __atomic_load_n(&(v)->counter, __ATOMIC_ACQUIRE)
#define atomic_try_cmpxchg(v, pold, n) __atomic_compare_exchange_n(&(v)->counter, (pold), (n), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_try_cmpxchg_relaxed atomic_try_cmpxchg
#define atomic_try_cmpxchg_acquire atomic_try_cmpxchg
#define atomic_try_cmpxchg_release atomic_try_cmpxchg
#define atomic64_try_cmpxchg atomic_try_cmpxchg
#define atomic_long_try_cmpxchg atomic_try_cmpxchg
#define try_cmpxchg(p, pold, n) __atomic_compare_exchange_n((p), (pold), (n), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define try_cmpxchg64 try_cmpxchg
#define try_cmpxchg_relaxed try_cmpxchg
#define spin_lock_irqsave_nested(lock, flags, subclass) spin_lock_irqsave(lock, flags)
#define overflows_type(n, T) ({ __typeof__(T) __ovf_v = 0; __builtin_add_overflow((n), 0, &__ovf_v); })

LKPI_EXTERN_C_END

#endif
