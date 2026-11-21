/* LinuxKPI: kernel interfaces the GPU drivers (TTM, i915, amdgpu, radeon,
 * nouveau, bochs) expect from the common headers. Included by lkpi_all.h. */
#ifndef _LKPI_LINUX_GPU_H
#define _LKPI_LINUX_GPU_H

#include <linux/ratelimit.h>

LKPI_EXTERN_C_BEGIN

struct pci_dev;
struct pci_device_id;
struct scatterlist;
struct device;
struct xarray;
struct xa_limit;

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
#ifndef offset_in_page
#define offset_in_page(p) ((unsigned long)(p) & ~PAGE_MASK)
#endif
static inline int get_order(unsigned long size)
{
	int order = 0;
	size = (size - 1) >> PAGE_SHIFT;
	while (size) {
		order++;
		size >>= 1;
	}
	return order;
}
#define ioread32be(a) __builtin_bswap32(readl(a))
#define iowrite32be(v, a) writel(__builtin_bswap32(v), a)
#define ioread16be(a) __builtin_bswap16(readw(a))
#define iowrite16be(v, a) writew(__builtin_bswap16(v), a)
#define ioread64(a) readq(a)
#define iowrite64(v, a) writeq(v, a)
#define container_of_const(ptr, type, member) container_of(ptr, type, member)

/* lockdep pinning (off) */
struct pin_cookie { unsigned int val; };
#define NIL_COOKIE (struct pin_cookie){ .val = 0U, }
#define lockdep_pin_lock(l) ({ struct pin_cookie __c = { 0 }; (void)(l); __c; })
#define lockdep_repin_lock(l, c) do { (void)(l); (void)(c); } while (0)
#define lockdep_unpin_lock(l, c) do { (void)(l); (void)(c); } while (0)
#define lockdep_assert_held_write(l) do { (void)(l); } while (0)
#define lockdep_assert_held_read(l) do { (void)(l); } while (0)
#define lockdep_assert_none_held_once() do { } while (0)
#define lockdep_assert_irqs_disabled() do { } while (0)
#define lockdep_assert_irqs_enabled() do { } while (0)
#define lockdep_assert_in_softirq() do { } while (0)
#define might_lock_nested(l, s) do { } while (0)
#define lock_map_acquire(m) do { } while (0)
#define lock_map_release(m) do { } while (0)
#define lock_map_acquire_try(m) do { } while (0)
#define lock_map_acquire_read(m) do { } while (0)
#define lock_acquire_shared_recursive(m, s, t, n, i) do { } while (0)
#define lockdep_set_subclass(l, s) do { } while (0)
#define lockdep_init_map(m, n, k, s) do { } while (0)
struct lockdep_map { int dummy; };
#define STATIC_LOCKDEP_MAP_INIT(n, k) { 0 }

/* ---- types & small helpers ---- */
typedef struct { u8 b[16]; } guid_t;
typedef struct { u8 b[16]; } uuid_t;
#define UUID_SIZE 16
#define UUID_STRING_LEN 36
#define GUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) ((guid_t){{ (a) & 0xff, ((a) >> 8) & 0xff, ((a) >> 16) & 0xff, ((a) >> 24) & 0xff, (b) & 0xff, ((b) >> 8) & 0xff, (c) & 0xff, ((c) >> 8) & 0xff, (d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) }})
#define UUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) ((uuid_t){{ ((a) >> 24) & 0xff, ((a) >> 16) & 0xff, ((a) >> 8) & 0xff, (a) & 0xff, ((b) >> 8) & 0xff, (b) & 0xff, ((c) >> 8) & 0xff, (c) & 0xff, (d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) }})
static inline bool guid_equal(const guid_t *u1, const guid_t *u2) { return __builtin_memcmp(u1, u2, sizeof(guid_t)) == 0; }
static inline bool uuid_equal(const uuid_t *u1, const uuid_t *u2) { return __builtin_memcmp(u1, u2, sizeof(uuid_t)) == 0; }
static inline void guid_copy(guid_t *dst, const guid_t *src) { __builtin_memcpy(dst, src, sizeof(guid_t)); }
static inline void uuid_copy(uuid_t *dst, const uuid_t *src) { __builtin_memcpy(dst, src, sizeof(uuid_t)); }
static inline bool guid_is_null(const guid_t *g) { for (int i = 0; i < 16; i++) if (g->b[i]) return false; return true; }
static inline bool uuid_is_null(const uuid_t *u) { for (int i = 0; i < 16; i++) if (u->b[i]) return false; return true; }
#define guid_gen(g) get_random_bytes(g, sizeof(guid_t))
#define uuid_gen(u) get_random_bytes(u, sizeof(uuid_t))
static inline void import_guid(guid_t *dst, const u8 *src) { __builtin_memcpy(dst, src, sizeof(guid_t)); }
static inline void export_guid(u8 *dst, const guid_t *src) { __builtin_memcpy(dst, src, sizeof(guid_t)); }
static inline void import_uuid(uuid_t *dst, const u8 *src) { __builtin_memcpy(dst, src, sizeof(uuid_t)); }
static inline void export_uuid(u8 *dst, const uuid_t *src) { __builtin_memcpy(dst, src, sizeof(uuid_t)); }
#ifndef _LKPI_PID_T
#define _LKPI_PID_T
typedef int pid_t;
#endif
#define __lkpi_type_max(T) ((u64)(__builtin_choose_expr((T)-1 < (T)0, (1ULL << (8 * sizeof(T) - 1)) - 1, \
	(sizeof(T) == 8 ? ~0ULL : (1ULL << (8 * sizeof(T))) - 1))))
#define castable_to_type(n, T) __builtin_choose_expr(__is_constexpr(n), \
	((n) >= 0 && (u64)(n) <= __lkpi_type_max(T)), __builtin_types_compatible_p(__typeof__(n), T))
#define KTIME_MAX ((s64)~((u64)1 << 63))
#define KTIME_MIN (-KTIME_MAX - 1)
#define KTIME_SEC_MAX (KTIME_MAX / NSEC_PER_SEC)
#define ktime_get_raw_fast_ns() ktime_get_ns()
#define ktime_get_raw_ns() ktime_get_ns()
#define ktime_get_mono_fast_ns() ktime_get_ns()
#define round_jiffies_up_relative(j) (j)
static inline void memset32(u32 *s, u32 v, size_t n) { while (n--) *s++ = v; }
static inline void memset64(u64 *s, u64 v, size_t n) { while (n--) *s++ = v; }
static inline void memset16(u16 *s, u16 v, size_t n) { while (n--) *s++ = v; }
#define str_enable_disable(v) ((v) ? "enable" : "disable")
#define str_on_off(v) ((v) ? "on" : "off")
#define str_yes_no(v) ((v) ? "yes" : "no")
#define str_true_false(v) ((v) ? "true" : "false")
#define str_high_low(v) ((v) ? "high" : "low")
#define str_read_write(v) ((v) ? "read" : "write")
#define str_plural(n) ((n) == 1 ? "" : "s")
#ifndef str_enabled_disabled
#define str_enabled_disabled(v) ((v) ? "enabled" : "disabled")
#endif
#define POISON_INUSE 0x5a
#define POISON_FREE 0x6b
#define POISON_END 0xa5
#define POISON_POINTER_DELTA 0
#define LIST_POISON1 ((void *)0x100)
#define LIST_POISON2 ((void *)0x122)
#ifndef __GFP_RECLAIM
#define __GFP_RECLAIM 0
#endif
#ifndef __GFP_RECLAIMABLE
#define __GFP_RECLAIMABLE 0
#endif
#ifndef __GFP_DIRECT_RECLAIM
#define __GFP_DIRECT_RECLAIM 0
#endif
#ifndef __GFP_KSWAPD_RECLAIM
#define __GFP_KSWAPD_RECLAIM 0
#endif
#ifndef __GFP_RETRY_MAYFAIL
#define __GFP_RETRY_MAYFAIL 0
#endif
#ifndef __GFP_NORETRY
#define __GFP_NORETRY 0
#endif
#ifndef __GFP_NOWARN
#define __GFP_NOWARN 0
#endif
#ifndef __GFP_HIGHMEM
#define __GFP_HIGHMEM 0
#endif
#ifndef __GFP_MOVABLE
#define __GFP_MOVABLE 0
#endif
#ifndef __GFP_COMP
#define __GFP_COMP 0
#endif
#define might_alloc(gfp) do { } while (0)
#define list_for_each_entry_lockless(pos, head, member) list_for_each_entry_rcu(pos, head, member)
#define SLAB_TYPESAFE_BY_RCU 0x00080000UL
#define SLAB_RECLAIM_ACCOUNT 0x00020000UL
#define SLAB_ACCOUNT 0x04000000UL
#define SLAB_PANIC 0x00040000UL
#define KMEM_CACHE(__struct, __flags) kmem_cache_create(#__struct, sizeof(struct __struct), __alignof__(struct __struct), (__flags), NULL)
#define kmem_cache_create_usercopy(n, s, a, f, uo, us, c) kmem_cache_create(n, s, a, f, c)
#define pat_enabled() true
#define _PAGE_BIT_PWT 3
#define _PAGE_BIT_PCD 4
#define _PAGE_PWT (1UL << _PAGE_BIT_PWT)
#define _PAGE_PCD (1UL << _PAGE_BIT_PCD)
#define _PAGE_CACHE_MASK (_PAGE_PWT | _PAGE_PCD)
#define PCI_D1 1
#define PCI_D2 2
#define PCI_UNKNOWN 5
#define DL_FLAG_STATELESS 0x01
#define DL_FLAG_AUTOREMOVE_CONSUMER 0x04
#define DL_FLAG_PM_RUNTIME 0x20
#define device_link_add(consumer, supplier, flags) ((struct device_link *)NULL)
#define device_link_del(link) do { } while (0)
#define device_link_remove(c, s) do { } while (0)
struct device_link;
#define FBINFO_STATE_RUNNING 0
#define FBINFO_STATE_SUSPENDED 1
#define mutex_lock_interruptible_nested(l, s) mutex_lock_interruptible(l)
#define mutex_lock_killable_nested(l, s) mutex_lock_killable(l)
#define mutex_acquire(m, s, t, i) do { } while (0)
#define mutex_release(m, i) do { } while (0)
#define mutex_acquire_nest(m, s, t, n, i) do { } while (0)
#define xa_lock_irqsave(xa, flags) spin_lock_irqsave(&(xa)->xa_lock, flags)
#define xa_unlock_irqrestore(xa, flags) spin_unlock_irqrestore(&(xa)->xa_lock, flags)
#define xa_lock_irq(xa) spin_lock_irq(&(xa)->xa_lock)
#define xa_unlock_irq(xa) spin_unlock_irq(&(xa)->xa_lock)
#define component_add_typed(dev, ops, type) 0
#define component_del_typed(dev, ops) do { } while (0)

/* ---- user memory: the kernel accesses user pointers directly after a check ---- */
#define access_ok(addr, size) lkpi_access_ok((const void __user *)(addr), (size))
bool lkpi_access_ok(const void __user *addr, unsigned long size);
#define __copy_from_user(to, from, n) copy_from_user(to, from, n)
#define __copy_to_user(to, from, n) copy_to_user(to, from, n)
#define __copy_from_user_inatomic(to, from, n) copy_from_user(to, from, n)
#define __copy_to_user_inatomic(to, from, n) copy_to_user(to, from, n)
#define copy_from_user_inatomic(to, from, n) copy_from_user(to, from, n)
#define __copy_from_user_inatomic_nocache(to, from, n) copy_from_user(to, from, n)
#define __get_user(x, p) get_user(x, p)
#define __put_user(x, p) put_user(x, p)
#define user_access_begin(p, n) access_ok(p, n)
#define user_access_end() do { } while (0)
#define user_read_access_begin(p, n) access_ok(p, n)
#define user_read_access_end() do { } while (0)
#define user_write_access_begin(p, n) access_ok(p, n)
#define user_write_access_end() do { } while (0)
#define unsafe_put_user(x, p, label) do { if (put_user(x, p)) goto label; } while (0)
#define unsafe_get_user(x, p, label) do { if (get_user(x, p)) goto label; } while (0)
#define unsafe_copy_to_user(d, s, n, label) do { if (copy_to_user(d, s, n)) goto label; } while (0)
#define unsafe_copy_from_user(d, s, n, label) do { if (copy_from_user(d, s, n)) goto label; } while (0)
#define pagefault_disable() do { } while (0)
#define pagefault_enable() do { } while (0)
#define faultin_page_readable(p, n) 0
#define fault_in_readable(p, n) 0
#define fault_in_writeable(p, n) 0
#define check_copy_size(addr, n, is_source) true

/* ---- batch 4 ---- */
#define U64_C(x) x##ULL
#define U32_C(x) x##U
#define DIV64_U64_ROUND_UP(ll, d) ({ u64 _tmp = (d); div64_u64((ll) + _tmp - 1, _tmp); })
#define DIV64_U64_ROUND_CLOSEST(ll, d) ({ u64 _tmp = (d); div64_u64((ll) + _tmp / 2, _tmp); })
#define offset_in_folio(f, p) ((unsigned long)(p) & (folio_size(f) - 1))
#define __mutex_init(l, name, key) mutex_init(l)
#define init_rcu_head(h) do { } while (0)
#define destroy_rcu_head(h) do { } while (0)
#define io_schedule_timeout(t) schedule_timeout(t)
#define io_schedule() schedule()
static inline void *memchr_inv(const void *start, int c, size_t bytes)
{
	const u8 *p = start;
	for (size_t i = 0; i < bytes; i++)
		if (p[i] != (u8)c)
			return (void *)(p + i);
	return NULL;
}
#define memcpy_from_page(to, page, offset, len) __builtin_memcpy((to), (char *)lkpi_page_address(page) + (offset), (len))
#define memcpy_to_page(page, offset, from, len) __builtin_memcpy((char *)lkpi_page_address(page) + (offset), (from), (len))
#define memzero_page(page, offset, len) __builtin_memset((char *)lkpi_page_address(page) + (offset), 0, (len))
void *memdup_user_nul(const void __user *src, size_t len);
#define atomic_dec_and_lock_irqsave(cnt, lock, flags) \
	({ bool __r = false; spin_lock_irqsave(lock, flags); if (atomic_dec_and_test(cnt)) __r = true; else spin_unlock_irqrestore(lock, flags); __r; })
#define WQ_UNBOUND_MAX_ACTIVE 512
#define WQ_MAX_ACTIVE 512
#define VM_MAP_PUT_PAGES 0x00000200
#define SWAP_CLUSTER_MAX 32UL
#define MAX_LFS_FILESIZE ((loff_t)LLONG_MAX)
#define IORES_DESC_NONE 0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#define PROT_NONE 0x0
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define vm_mmap(file, addr, len, prot, flag, offset) ((unsigned long)-ENODEV)
#define IOMEM_ERR_PTR(err) ((void __iomem *)ERR_PTR(err))
#define get_file_active(pfile) lkpi_get_file_active(pfile)
struct file *lkpi_get_file_active(struct file **f);
#define _PAGE_BIT_PAT 7
#define _PAGE_PAT (1UL << _PAGE_BIT_PAT)
#define BACKLIGHT_POWER_ON 0
#define BACKLIGHT_POWER_OFF 4
#define BACKLIGHT_POWER_REDUCED 1
typedef int suspend_state_t;
#define PCI_STD_RESOURCES 0
#define PCI_STD_RESOURCE_END 5
#define PCIBIOS_MIN_MEM 0x10000000
#define pcibios_align_resource NULL
#define pci_bus_alloc_resource(bus, res, size, align, min, type, alignf, data) (-ENODEV)
#define pcie_find_root_port(pdev) ((struct pci_dev *)NULL)
#define pci_physfn(pdev) (pdev)
#define XA_FLAGS_LOCK_IRQ 0x1
#define xa_alloc_cyclic_irq(xa, id, entry, limit, next, gfp) xa_alloc_cyclic(xa, id, entry, limit, next, gfp)
int lkpi_xa_alloc_cyclic(struct xarray *xa, u32 *id, void *entry, u32 min, u32 max, u32 *next, gfp_t gfp);
#define xa_alloc_cyclic(xa, id, entry, limit, next, gfp) lkpi_xa_alloc_cyclic(xa, id, entry, (limit).min, (limit).max, next, gfp)

/* ---- batch 5 ---- */
#include <linux/wait_bit.h>
#define PTR_IF(cond, ptr) ((cond) ? (ptr) : NULL)
#define PCI_DEVID(bus, devfn) ((((u16)(bus)) << 8) | (devfn))
#define time_after32(a, b) ((s32)((u32)(b) - (u32)(a)) < 0)
#define time_before32(b, a) time_after32(a, b)
#define strchrnul lkpi_strchrnul
static inline char *lkpi_strchrnul(const char *s, int c) { while (*s && *s != (char)c) s++; return (char *)s; }
#define vscnprintf(buf, size, fmt, args) ({ int __n = vsnprintf(buf, size, fmt, args); (size) ? min_t(int, __n, (int)(size) - 1) : 0; })
#define spin_trylock_irqsave(lock, flags) ({ local_irq_save(flags); spin_trylock(lock) ? 1 : ({ local_irq_restore(flags); 0; }); })
#define spin_trylock_irq(lock) spin_trylock(lock)
#define synchronize_hardirq(irq) (synchronize_irq(irq), true)
#define synchronize_srcu_expedited(s) synchronize_rcu()
#define get_state_synchronize_rcu() 0UL
#define poll_state_synchronize_rcu(c) true
#define cond_synchronize_rcu(c) synchronize_rcu()
#define register_vmap_purge_notifier(nb) 0
#define unregister_vmap_purge_notifier(nb) 0
#define need_resched() false
#define yield() schedule()
#define cond_resched_lock(lock) 0
#define on_each_cpu(func, info, wait) ((func)(info))
#define mutex_lock_nest_lock(l, nest) mutex_lock(l)
#define cmpxchg64(p, o, n) cmpxchg(p, o, n)
#define atomic_andnot(i, v) atomic_and(~(i), v)
#define atomic_long_andnot(i, v) atomic_and(~(i), v)
#define memset_p(p, v, n) do { void **__p = (void **)(p); for (size_t __i = 0; __i < (n); __i++) __p[__i] = (v); } while (0)
#define get_task_comm(buf, t) strscpy(buf, (t)->comm, sizeof(buf))
#define kref_put_lock(kref, release, lock) \
	({ int __r = 0; if (refcount_dec_and_test(&(kref)->refcount)) { spin_lock(lock); release(kref); __r = 1; } __r; })
#define refcount_dec_and_lock_irqsave(r, lock, flagsp) \
	({ bool __r = false; if (refcount_dec_and_test(r)) { spin_lock_irqsave(lock, *(flagsp)); __r = true; } __r; })
#define dev_notice_ratelimited dev_notice
#define device_iommu_mapped(dev) false
#define device_get_next_child_node(dev, child) ((struct fwnode_handle *)NULL)
#define INIT_DELAYED_WORK_ONSTACK(w, f) INIT_DELAYED_WORK(w, f)
#define destroy_delayed_work_on_stack(w) do { } while (0)
#define destroy_work_on_stack(w) do { } while (0)
#define INIT_WORK_ONSTACK(w, f) INIT_WORK(w, f)
struct rcu_work { struct work_struct work; struct rcu_head rcu; struct workqueue_struct *wq; };
void lkpi_rcu_work_fn(struct rcu_head *h);
#define INIT_RCU_WORK(rw, f) INIT_WORK(&(rw)->work, f)
static inline bool queue_rcu_work(struct workqueue_struct *wq, struct rcu_work *rw)
{
	rw->wq = wq;
	call_rcu(&rw->rcu, lkpi_rcu_work_fn);
	return true;
}
#define to_rcu_work(w) container_of(w, struct rcu_work, work)
#define kstrtobool_from_user(s, count, res) lkpi_kstrtobool_from_user(s, count, res)
int lkpi_kstrtobool_from_user(const char __user *s, size_t count, bool *res);
#define hex_dump_to_buffer(buf, len, rowsize, groupsize, linebuf, linebuflen, ascii) \
	snprintf(linebuf, linebuflen, "%*ph", (int)(len), buf)
static inline u64 mul_u64_u32_div(u64 a, u32 mul, u32 divisor)
{
	/* Linux's generic version: no 128-bit division in the kernel. */
	u64 rl = (u64)(u32)a * mul;
	u64 rh = (u64)(u32)(a >> 32) * mul + (rl >> 32);
	u64 qh = rh / divisor, rem = rh % divisor;
	rl = (rem << 32) | (u32)rl;
	return (qh << 32) + rl / divisor;
}
#define bitmap_andnot(dst, s1, s2, nbits) ({ for (unsigned int __i = 0; __i < BITS_TO_LONGS(nbits); __i++) (dst)[__i] = (s1)[__i] & ~(s2)[__i]; true; })
int bitmap_find_free_region(unsigned long *bitmap, unsigned int bits, int order);
void bitmap_release_region(unsigned long *bitmap, unsigned int pos, int order);
void bitmap_shift_right(unsigned long *dst, const unsigned long *src, unsigned int shift, unsigned int nbits);
#define sort_r(base, num, size, cmp, swap, priv) lkpi_sort_r(base, num, size, cmp, priv)
void lkpi_sort_r(void *base, size_t num, size_t size, int (*cmp)(const void *, const void *, const void *), const void *priv);
#define memcpy_to_folio(f, off, from, len) __builtin_memcpy((char *)lkpi_page_address(&(f)->page) + (off), (from), (len))
#define sg_set_folio(sg, folio, len, offset) sg_set_page(sg, &(folio)->page, len, offset)
#define page_mapped(p) false
#define PageWriteback(p) false
#define SetPageReclaim(p) do { } while (0)
#define ClearPageReclaim(p) do { } while (0)
#define clear_page_dirty_for_io(p) ((p)->flags &= ~PG_dirty, 1)
#define find_lock_page(mapping, idx) ((struct page *)NULL)
#define unlock_page(p) do { } while (0)
#define lock_page(p) do { } while (0)
#define trylock_page(p) true
#define shmem_file_setup_with_mnt(mnt, name, size, flags) shmem_file_setup(name, size, flags)
struct vfsmount;
#define find_vma(mm, addr) ((struct vm_area_struct *)NULL)
#define mmap_write_lock_killable(mm) 0
#define call_mmap(file, vma) ((file)->f_op->mmap(file, vma))
#define xa_mk_value(v) ((void *)((((unsigned long)(v)) << 1) | 1))
#define xa_to_value(e) (((unsigned long)(e)) >> 1)
#define xa_is_value(e) (((unsigned long)(e)) & 1)
#define xa_erase_irq(xa, i) xa_erase(xa, i)
#define pci_match_id(ids, dev) lkpi_pci_match_id(ids, dev)
const struct pci_device_id *lkpi_pci_match_id(const struct pci_device_id *ids, struct pci_dev *dev);
#define pci_bus_for_each_resource(bus, res, ...) for (res = NULL; res; )
#define dma_map_sg_attrs(dev, sg, n, dir, attrs) lkpi_dma_map_sg(dev, sg, n, dir)
#define dma_map_sg(dev, sg, n, dir) lkpi_dma_map_sg(dev, sg, n, dir)
#define dma_unmap_sg(dev, sg, n, dir) do { } while (0)
#define dma_unmap_sg_attrs(dev, sg, n, dir, attrs) do { } while (0)
int lkpi_dma_map_sg(struct device *dev, struct scatterlist *sg, int nents, int dir);
#define devm_pinctrl_get_select(dev, state) ((struct pinctrl *)ERR_PTR(-ENODEV))
struct pinctrl;
#define i2c_acpi_find_adapter_by_handle(h) ((struct i2c_adapter *)NULL)
#define pwm_is_enabled(p) false
#define relay_buf_full(b) 0
#define to_auxiliary_dev(d) container_of(d, struct auxiliary_device, dev)

/* ---- radeon / amdgpu / nouveau ---- */
struct dev_pm_domain { struct dev_pm_ops ops; void (*detach)(struct device *, bool); };
#ifndef S_IRUGO
#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IRUGO (S_IRUSR | S_IRGRP | S_IROTH)
#define S_IWUGO (S_IWUSR | S_IWGRP | S_IWOTH)
#endif
#define i2c_new_client_device(adap, info) ((struct i2c_client *)ERR_PTR(-ENODEV))
#define i2c_unregister_device(c) do { } while (0)
#define down_read_trylock(s) mutex_trylock(&(s)->m)
#define down_write_trylock(s) mutex_trylock(&(s)->m)
#define downgrade_write(s) do { } while (0)
#define acpi_format_exception(s) "ACPI error"
#define IRQ_NOTCONNECTED (1U << 31)
#define FOLL_WRITE 0x01
#define FOLL_LONGTERM 0x100
#define untagged_addr(a) (a)
#define set_memory_uc(addr, n) 0
#define set_memory_wt(addr, n) 0
#define release_pages(pages, n) do { for (int __i = 0; __i < (int)(n); __i++) put_page((pages)[__i]); } while (0)
#define unpin_user_pages(pages, n) release_pages(pages, n)
#define pcie_set_readrq(d, rq) 0
#define pcie_get_readrq(d) 512
#define pcie_get_mps(d) 128
#define pcie_set_mps(d, mps) 0
#define mmu_notifier_synchronize() do { } while (0)
#define get_user_pages(start, n, flags, pages) (-EFAULT)
#define pin_user_pages_fast(start, n, flags, pages) (-EFAULT)
#define get_user_pages_fast(start, n, flags, pages) (-EFAULT)
#define atomic64_xchg(v, n) __atomic_exchange_n(&(v)->counter, (n), __ATOMIC_SEQ_CST)
#define atomic64_cmpxchg(v, o, n) ({ __typeof__((v)->counter + 0) __o = (o); __atomic_compare_exchange_n(&(v)->counter, &__o, (n), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); __o; })

/* ---- amdgpu ---- */
#include <linux/rbtree.h>
#define atomic_long_cmpxchg(v, o, n) atomic64_cmpxchg(v, o, n)
#define atomic_long_fetch_add atomic_fetch_add
#define memalloc_noreclaim_save() 0U
#define memalloc_noreclaim_restore(f) do { (void)(f); } while (0)
#define memalloc_nofs_save() 0U
#define memalloc_nofs_restore(f) do { (void)(f); } while (0)
#define memalloc_noio_save() 0U
#define memalloc_noio_restore(f) do { (void)(f); } while (0)
#define PF_EXITING 0x00000004
#define PF_KTHREAD 0x00200000
#ifndef SIGKILL
#define SIGKILL 9
#endif
#define devres_open_group(dev, id, gfp) ((void *)1)
#define devres_release_group(dev, id) 0
#define devres_remove_group(dev, id) do { } while (0)
#define devres_close_group(dev, id) do { } while (0)

/* ---- amdgpu 2 ---- */
#include <linux/sysfs.h>
typedef unsigned int pci_ers_result_t;
typedef unsigned int pci_channel_state_t;
enum { PCI_ERS_RESULT_NONE = 1, PCI_ERS_RESULT_CAN_RECOVER = 2, PCI_ERS_RESULT_NEED_RESET = 3,
       PCI_ERS_RESULT_DISCONNECT = 4, PCI_ERS_RESULT_RECOVERED = 5, PCI_ERS_RESULT_NO_AER_DRIVER = 6 };
enum { pci_channel_io_normal = 1, pci_channel_io_frozen = 2, pci_channel_io_perm_failure = 3 };
struct pci_error_handlers {
	pci_ers_result_t (*error_detected)(struct pci_dev *dev, pci_channel_state_t error);
	pci_ers_result_t (*mmio_enabled)(struct pci_dev *dev);
	pci_ers_result_t (*slot_reset)(struct pci_dev *dev);
	void (*reset_prepare)(struct pci_dev *dev);
	void (*reset_done)(struct pci_dev *dev);
	void (*resume)(struct pci_dev *dev);
	void (*cor_error_detected)(struct pci_dev *dev);
};
#define rwsem_is_contended(s) 0
#define rwsem_is_locked(s) mutex_is_locked(&(s)->m)
#define HW_ERR "[Hardware Error]: "
#define printk_ratelimit() 1
#define printk_ratelimited printk
#ifndef FALSE
#define FALSE 0
#define TRUE 1
#endif
#define list_for_each_prev_safe(pos, n, head) for (pos = (head)->prev, n = pos->prev; pos != (head); pos = n, n = pos->prev)
#define dev_WARN(dev, fmt, ...) WARN(1, "%s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_WARN_ONCE(dev, cond, fmt, ...) WARN_ONCE(cond, "%s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define xen_initial_domain() 0
#define xen_domain() 0
typedef unsigned long irq_hw_number_t;
/* kobjects / ksets (sysfs directories): accepted, not exposed. */
struct kobj_type { void (*release)(struct kobject *kobj); const struct sysfs_ops *sysfs_ops; const struct attribute_group **default_groups; };
struct sysfs_ops { ssize_t (*show)(struct kobject *, struct attribute *, char *); ssize_t (*store)(struct kobject *, struct attribute *, const char *, size_t); };
struct kset { struct list_head list; spinlock_t list_lock; struct kobject kobj; };
#define to_kset(k) container_of(k, struct kset, kobj)
extern const struct sysfs_ops kobj_sysfs_ops;
#define kobject_init(k, t) do { (void)(t); } while (0)
#define kobject_init_and_add(k, t, parent, fmt, ...) 0
#define kobject_add(k, parent, fmt, ...) 0
#define kobject_put(k) do { (void)(k); } while (0)
#define kobject_get(k) (k)
#define kobject_del(k) do { (void)(k); } while (0)
#define kobject_set_name(k, fmt, ...) 0
#define kobject_create_and_add(name, parent) ((struct kobject *)NULL)
#define kset_register(k) 0
#define kset_unregister(k) do { } while (0)
#define kset_create_and_add(name, ops, parent) ((struct kset *)NULL)
#define sysfs_remove_file_from_group(k, a, g) do { } while (0)
#define sysfs_add_file_to_group(k, a, g) 0
#define sysfs_create_link(k, t, n) 0
#define sysfs_remove_link(k, n) do { } while (0)

/* ---- amdgpu 3 ---- */
#include <linux/sizes.h>
#define cpu_data(cpu) boot_cpu_data
#ifndef X86_VENDOR_INTEL
#define X86_VENDOR_INTEL 0
#define X86_VENDOR_AMD 2
#endif
#define topology_num_cores_per_package() 1
#define num_possible_nodes() 1
#define num_online_nodes() 1
#define pfn_to_nid(pfn) 0
#define page_is_ram(pfn) 1
#define dev_is_removable(dev) false
#define devm_device_add_group(dev, grp) 0
#define device_add_group(dev, grp) 0
#define device_remove_group(dev, grp) do { } while (0)
#define pci_wait_for_pending_transaction(pdev) 1
#define pci_device_is_present(pdev) true
#define pci_pr3_present(pdev) false
#define PCI_POSSIBLE_ERROR(val) ((val) == ((__typeof__(val))~0))
#define PCI_ERROR_RESPONSE (~0ULL)
struct pci_saved_state;
#define pci_store_saved_state(pdev) ((struct pci_saved_state *)NULL)
#define pci_load_saved_state(pdev, st) 0
#define pci_load_and_free_saved_state(pdev, st) 0
#define pci_restore_msi_state(pdev) do { } while (0)
#define list_rotate_to_front(list, head) list_move_tail(head, list)
#define radix_tree_tag_set(root, idx, tag) ((void *)NULL)
#define radix_tree_tag_clear(root, idx, tag) ((void *)NULL)
#define radix_tree_tagged(root, tag) 0
#define radix_tree_gang_lookup_tag(root, results, first, max, tag) 0U
#define pm_runtime_autosuspend_expiration(dev) 0UL
#define ksys_sync_helper() do { } while (0)
#define emergency_restart() do { } while (0)
#define kobject_name(k) ((k)->name)
#define d_inode(d) ((d)->d_inode)
#define backlight_device_set_brightness(bd, b) 0
#define bitmap_complement(dst, src, nbits) ({ for (unsigned int __i = 0; __i < BITS_TO_LONGS(nbits); __i++) (dst)[__i] = ~(src)[__i]; })
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
typedef void *fl_owner_t;
#define return_ACPI_STATUS(s) return (s)
#ifndef VM_ACCESS_FLAGS
#define VM_ACCESS_FLAGS (VM_READ | VM_WRITE | VM_EXEC)
#endif
#define sysfs_bin_attr_init(a) do { } while (0)
#define BIN_ATTR(_name, _mode, _read, _write, _size) struct bin_attribute bin_attr_##_name = { .attr = { .name = __stringify(_name), .mode = _mode }, .read = _read, .write = _write, .size = _size }
#define BIN_ATTR_RO(_name, _size) BIN_ATTR(_name, 0444, _name##_read, NULL, _size)
#define BIN_ATTR_RW(_name, _size) BIN_ATTR(_name, 0644, _name##_read, _name##_write, _size)
#define __BIN_ATTR(_name, _mode, _read, _write, _size) { .attr = { .name = __stringify(_name), .mode = _mode }, .read = _read, .write = _write, .size = _size }
int kstrtouint_from_user(const char __user *s, size_t count, unsigned int base, unsigned int *res);
#define kstrtou64(s, base, res) kstrtoull(s, base, (unsigned long long *)(res))

LKPI_EXTERN_C_END

#endif
