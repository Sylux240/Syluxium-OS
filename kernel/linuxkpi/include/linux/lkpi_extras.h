/* LinuxKPI: assorted definitions the DRM core (and other larger Linux
 * subsystems) expect to be reachable from the common headers. */
#pragma once

#include <linux/completion.h>
#include <stdarg.h>

typedef unsigned long __kernel_size_t;
typedef long __kernel_ssize_t;
typedef long __kernel_ptrdiff_t;
typedef long __kernel_off_t;
typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned long ulong;

#define raw_spinlock spinlock
#ifndef TASK_COMM_LEN
#define TASK_COMM_LEN 16
#endif
#define _THIS_IP_ ({ __label__ __here; __here: (unsigned long)&&__here; })
#define in_dbg_master() 0
#ifndef __malloc
#define __malloc __attribute__((__malloc__))
#endif
#ifndef PAGE_ALIGN
#define PAGE_ALIGN(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(x) ((x) & ~(PAGE_SIZE - 1))
#endif

/* printk's "%pV": a nested format + arguments. */
struct va_format { const char *fmt; va_list *va; };

/* Stack depot (debug stack traces): unused. */
typedef unsigned int depot_stack_handle_t;
static inline depot_stack_handle_t stack_depot_save(unsigned long *e, unsigned int n, unsigned int f) { return 0; }
static inline void stack_depot_init(void) { }
static inline int stack_depot_snprint(depot_stack_handle_t h, char *buf, size_t size, int spaces) { return 0; }
static inline unsigned int stack_trace_save(unsigned long *store, unsigned int size, unsigned int skip) { return 0; }

/* Sequence locks: a spinlock and a counter. */
typedef struct { spinlock_t lock; unsigned int seq; } seqlock_t;
typedef struct { unsigned int seq; } seqcount_t;
#define seqlock_init(sl) do { spin_lock_init(&(sl)->lock); (sl)->seq = 0; } while (0)
#define DEFINE_SEQLOCK(x) seqlock_t x = { .seq = 0 }
static inline unsigned int read_seqbegin(const seqlock_t *sl) { return __atomic_load_n(&sl->seq, __ATOMIC_ACQUIRE) & ~1u; }
static inline unsigned int read_seqretry(const seqlock_t *sl, unsigned int start)
{
	return __atomic_load_n(&sl->seq, __ATOMIC_ACQUIRE) != start;
}
#define write_seqlock(sl) do { spin_lock(&(sl)->lock); __atomic_add_fetch(&(sl)->seq, 1, __ATOMIC_RELEASE); } while (0)
#define write_sequnlock(sl) do { __atomic_add_fetch(&(sl)->seq, 1, __ATOMIC_RELEASE); spin_unlock(&(sl)->lock); } while (0)
#define write_seqlock_irqsave(sl, flags) do { (flags) = 0; write_seqlock(sl); } while (0)
#define write_sequnlock_irqrestore(sl, flags) write_sequnlock(sl)
#define seqcount_init(s) ((s)->seq = 0)

/* kmsg dumpers (panic log savers): never called. */
enum kmsg_dump_reason { KMSG_DUMP_UNDEF, KMSG_DUMP_PANIC, KMSG_DUMP_OOPS, KMSG_DUMP_EMERG, KMSG_DUMP_SHUTDOWN, KMSG_DUMP_MAX };
struct kmsg_dump_detail { enum kmsg_dump_reason reason; const char *description; };
struct kmsg_dumper { struct list_head list; void (*dump)(struct kmsg_dumper *, struct kmsg_dump_detail *); enum kmsg_dump_reason max_reason; bool registered; };
static inline int kmsg_dump_register(struct kmsg_dumper *d) { return 0; }
static inline int kmsg_dump_unregister(struct kmsg_dumper *d) { return 0; }

/* "nomodeset" on the kernel command line. */
static inline bool video_firmware_drivers_only(void) { return false; }

#define dev_driver_string(dev) ((dev) && (dev)->driver && (dev)->driver->name ? (dev)->driver->name : "")

/* kthread workers, on top of a workqueue. */
struct kthread_work;
typedef void (*kthread_work_func_t)(struct kthread_work *work);
struct kthread_worker { struct workqueue_struct *wq; };
struct kthread_work { struct work_struct work; kthread_work_func_t func; struct kthread_worker *worker; };
static inline void lkpi_kthread_work_fn(struct work_struct *w)
{
	struct kthread_work *kw = container_of(w, struct kthread_work, work);
	kw->func(kw);
}
#define kthread_init_work(kw, fn) do { INIT_WORK(&(kw)->work, lkpi_kthread_work_fn); (kw)->func = (fn); } while (0)
struct kthread_worker *kthread_create_worker(unsigned int flags, const char namefmt[], ...);
static inline bool kthread_queue_work(struct kthread_worker *w, struct kthread_work *kw)
{
	kw->worker = w;
	return queue_work(w->wq, &kw->work);
}
static inline void kthread_flush_work(struct kthread_work *kw) { flush_work(&kw->work); }
static inline bool kthread_cancel_work_sync(struct kthread_work *kw) { return cancel_work_sync(&kw->work); }
static inline void kthread_flush_worker(struct kthread_worker *w) { flush_workqueue(w->wq); }
void kthread_destroy_worker(struct kthread_worker *w);

/* ---- batch 2: DRM core needs ---- */
#define unmap_mapping_range(mapping, start, len, even_cows) do { } while (0)
typedef u64 resource_size_t;
struct resource { resource_size_t start, end; const char *name; unsigned long flags; struct resource *parent, *sibling, *child; };
#define resource_size(r) ((r)->end - (r)->start + 1)
#define DEFINE_RES_MEM(_start, _size) { .start = (_start), .end = (_start) + (_size) - 1, .flags = 0x200 }
#define IORESOURCE_MEM 0x00000200
struct debugfs_reg32 { char *name; unsigned long offset; };
struct debugfs_regset32 { const struct debugfs_reg32 *regs; int nregs; void __iomem *base; struct device *dev; };

#define CAP_SYS_ADMIN 21
#define CAP_SYS_NICE 23
bool lkpi_capable(int cap);
#define capable(cap) lkpi_capable(cap)
#define perfmon_capable() lkpi_capable(CAP_SYS_ADMIN)

static inline ssize_t strscpy_pad(char *dst, const char *src, size_t count)
{
	size_t i = 0;
	if (!count)
		return -E2BIG;
	for (; i + 1 < count && src[i]; i++)
		dst[i] = src[i];
	memset(dst + i, 0, count - i);
	return src[i] ? -E2BIG : (ssize_t)i;
}
long simple_strtol(const char *cp, char **endp, unsigned int base);
unsigned long simple_strtoul(const char *cp, char **endp, unsigned int base);
char *kvasprintf(gfp_t gfp, const char *fmt, va_list ap);

/* x86 cache control (GPU drivers flush CPU caches around DMA). */
#define X86_FEATURE_CLFLUSH 1
#define static_cpu_has(f) true
#define boot_cpu_has(f) true
static inline void clflushopt(volatile void *p) { __asm__ volatile("clflush %0" : "+m"(*(volatile char *)p)); }
#define clflush clflushopt
int wbinvd_on_all_cpus(void);
static inline void wbinvd(void) { __asm__ volatile("wbinvd" ::: "memory"); }
#define boot_cpu_data_clflush_size 64

/* Processes as DRM clients see them. */
enum pid_type { PIDTYPE_PID, PIDTYPE_TGID, PIDTYPE_PGID, PIDTYPE_SID, PIDTYPE_MAX };
struct pid { int nr; int refs; };
struct pid *lkpi_task_pid(struct task_struct *t);
#define task_tgid(t) lkpi_task_pid(t)
#define task_pid(t) lkpi_task_pid(t)
#define get_task_pid(t, type) lkpi_task_pid(t)
#define get_pid(p) (p)
#define put_pid(p) do { } while (0)
#define pid_nr(p) ((p) ? (p)->nr : 0)
#define pid_vnr(p) pid_nr(p)
#define task_pid_nr(t) ((t)->pid)
#define task_tgid_nr(t) ((t)->tgid)
#define task_tgid_vnr(t) ((t)->tgid)
#define pid_task(p, type) ((struct task_struct *)NULL)

/* SRCU: plain RCU here. */
struct srcu_struct { int unused; };
#define DEFINE_STATIC_SRCU(name) static struct srcu_struct name
#define DEFINE_SRCU(name) struct srcu_struct name
#define srcu_read_lock(s) ({ (void)(s); rcu_read_lock(); 0; })
#define srcu_read_unlock(s, idx) do { (void)(s); (void)(idx); rcu_read_unlock(); } while (0)
#define synchronize_srcu(s) synchronize_rcu()
#define init_srcu_struct(s) 0
#define cleanup_srcu_struct(s) do { } while (0)

/* sysfs class attributes: not exported. */
struct class_attribute_string { int unused; };
#define CLASS_ATTR_STRING(_name, _mode, _str) struct class_attribute_string class_attr_##_name = { 0 }
#define class_create_file(cls, attr) 0
#define class_remove_file(cls, attr) do { } while (0)

#ifndef module_platform_driver
#define module_platform_driver(drv) \
	static int __init drv##_lkpi_init(void) { return platform_driver_register(&(drv)); } \
	module_init(drv##_lkpi_init)
#endif

/* Scatter/gather tables. */
struct scatterlist;
struct sg_table { struct scatterlist *sgl; unsigned int nents; unsigned int orig_nents; };
enum dma_data_direction;
struct device;
int dma_map_sgtable(struct device *dev, struct sg_table *sgt, enum dma_data_direction dir, unsigned long attrs);
void dma_unmap_sgtable(struct device *dev, struct sg_table *sgt, enum dma_data_direction dir, unsigned long attrs);
#define DMA_ATTR_SKIP_CPU_SYNC (1UL << 5)
#define dma_max_mapping_size(dev) SIZE_MAX
int sg_alloc_table(struct sg_table *t, unsigned int nents, gfp_t gfp);
void sg_free_table(struct sg_table *t);

/* ---- batch 3: DRM core long tail ---- */
#define oops_in_progress 0
#define overflowuid 65534
#ifndef O_EXCL
#define O_EXCL 00000200
#endif
#define ARCH_KMALLOC_MINALIGN 8
#define ARCH_DMA_MINALIGN 64
#define MINORBITS 20
#define MINORMASK ((1U << MINORBITS) - 1)
#define MKDEV(ma, mi) (((ma) << MINORBITS) | (mi))
#define MAJOR(dev) ((unsigned int)((dev) >> MINORBITS))
#define MINOR(dev) ((unsigned int)((dev) & MINORMASK))
#define old_encode_dev(d) ((u16)((MAJOR(d) << 8) | MINOR(d)))
#define new_encode_dev(d) ((u32)((MINOR(d) & 0xff) | (MAJOR(d) << 8) | ((MINOR(d) & ~0xff) << 12)))
#ifndef SZ_1K
#define SZ_1K 0x00000400
#define SZ_4K 0x00001000
#define SZ_64K 0x00010000
#define SZ_1M 0x00100000
#define SZ_2M 0x00200000
#define SZ_16M 0x01000000
#define SZ_256M 0x10000000
#define SZ_1G 0x40000000
#endif
#define MAX_T(type, a, b) ((type)(a) > (type)(b) ? (type)(a) : (type)(b))
#define MIN_T(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define swab16(x) __builtin_bswap16(x)
#define swab32(x) __builtin_bswap32(x)
#define swab64(x) __builtin_bswap64(x)
static inline u64 div64_u64_rem(u64 dividend, u64 divisor, u64 *remainder)
{
	*remainder = dividend % divisor;
	return dividend / divisor;
}
#define nsecs_to_jiffies64(n) ((u64)(n) / (1000000000ULL / HZ))
static inline char *strnchr(const char *s, size_t count, int c)
{
	for (; count-- && *s; s++)
		if (*s == (char)c)
			return (char *)s;
	return NULL;
}
static inline size_t str_has_prefix(const char *str, const char *prefix)
{
	size_t len = strlen(prefix);
	return strncmp(str, prefix, len) == 0 ? len : 0;
}
static inline bool sysfs_streq(const char *s1, const char *s2)
{
	while (*s1 && *s1 == *s2) {
		s1++;
		s2++;
	}
	if (*s1 == *s2)
		return true;
	if (!*s1 && *s2 == '\n' && !s2[1])
		return true;
	if (*s1 == '\n' && !s1[1] && !*s2)
		return true;
	return false;
}
void sort(void *base, size_t num, size_t size, int (*cmp)(const void *, const void *), void (*swap)(void *, void *, int));
struct list_head;
void list_sort(void *priv, struct list_head *head, int (*cmp)(void *, const struct list_head *, const struct list_head *));
#define __list_del_entry(e) list_del(e)
#define INIT_WORK_ONSTACK(w, f) INIT_WORK(w, f)
#define destroy_work_on_stack(w) do { } while (0)
#define lockdep_assert_once(c) do { } while (0)
#define lockdep_assert_none_held_once() do { } while (0)
#define compat_ptr(x) ((void __user *)(unsigned long)(x))
#define task_pid_vnr(t) ((t)->pid)
#define kmalloc_node_track_caller(size, gfp, node) kmalloc(size, gfp)
void *memdup_array_user(const void __user *src, size_t n, size_t size);
unsigned long clear_user(void __user *to, unsigned long n);
static inline int atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock)
{
	mutex_lock(lock);
	if (atomic_dec_return(cnt) == 0)
		return 1;
	mutex_unlock(lock);
	return 0;
}
#define kernel_fpu_begin() do { } while (0)
#define kernel_fpu_end() do { } while (0)
#define sched_set_fifo(t) do { } while (0)
#define sched_set_fifo_low(t) do { } while (0)

/* wait_event_lock_irq: the lock is dropped while waiting. */
#define wait_event_lock_irq(wq, cond, lock) \
	do { while (!(cond)) { spin_unlock_irq(&(lock)); wait_event(wq, (cond)); spin_lock_irq(&(lock)); } } while (0)

/* eventfd (DRM syncobj event notifications): not supported yet. */
struct eventfd_ctx;
#define eventfd_signal(ctx) do { } while (0)
#define eventfd_ctx_fdget(fd) ((struct eventfd_ctx *)ERR_PTR(-EINVAL))
#define eventfd_ctx_put(ctx) do { } while (0)

#define fops_get(fops) (fops)
#define fops_put(fops) do { } while (0)
#define replace_fops(f, fops) do { (f)->f_op = (fops); } while (0)
#define file_clone_open(f) ((struct file *)ERR_PTR(-EOPNOTSUPP))
#define simple_pin_fs(type, mount, count) 0
#define simple_release_fs(mount, count) do { } while (0)

/* Machine/platform queries. */
struct cpuinfo_x86 { int x86_clflush_size; int x86_cache_alignment; };
extern struct cpuinfo_x86 boot_cpu_data;
#define xen_pv_domain() 0
#define CC_ATTR_MEM_ENCRYPT 0
#define cc_platform_has(attr) false
#define vga_switcheroo_process_delayed_switch() do { } while (0)
struct component_ops { int (*bind)(struct device *, struct device *, void *); void (*unbind)(struct device *, struct device *, void *); };
#define component_add(dev, ops) 0
#define component_del(dev, ops) do { } while (0)
#define aperture_remove_conflicting_devices(base, size, name) 0
#define aperture_remove_conflicting_pci_devices(pdev, name) 0
#define devm_aperture_acquire_for_platform_device(pdev, base, size) 0

/* Device tree (PCs have none). */
struct device_node;
#define of_parse_phandle(np, name, index) ((struct device_node *)NULL)
#define of_property_read_u32(np, name, out) (-EINVAL)
#define of_property_read_string(np, name, out) (-EINVAL)
#define of_property_present(np, name) false
#define of_node_put(np) do { } while (0)
#define of_address_to_resource(np, index, r) (-EINVAL)
#define dev_fwnode(dev) ((struct fwnode_handle *)NULL)
#define dev_is_platform(dev) false
#define platform_get_resource(pdev, type, num) ((struct resource *)NULL)
#define kobj_to_dev(k) container_of(k, struct device, kobj)
struct kobject; struct file;
struct bin_attribute {
	struct attribute attr;
	size_t size;
	void *private;
	ssize_t (*read)(struct file *, struct kobject *, struct bin_attribute *, char *, loff_t, size_t);
	ssize_t (*write)(struct file *, struct kobject *, struct bin_attribute *, char *, loff_t, size_t);
};

/* Memory regions of devices. */
#define MEMREMAP_WB 1
#define MEMREMAP_WT 2
#define MEMREMAP_WC 4
extern struct resource iomem_resource;
#define devm_request_mem_region(dev, start, n, name) ((struct resource *)NULL)
void *devm_memremap(struct device *dev, resource_size_t offset, size_t size, unsigned long flags);
void __iomem *devm_ioremap_wc(struct device *dev, resource_size_t offset, resource_size_t size);
#define devm_add_action_or_reset(dev, action, data) ({ (void)(action); 0; })
#define devm_add_action(dev, action, data) ({ (void)(action); 0; })

/* PCI addresses. */
#define PCI_SLOT(devfn) (((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn) ((devfn) & 0x07)
#define pci_domain_nr(bus) 0
#define DECLARE_DYNDBG_CLASSMAP(...)

/* ---- batch 4 ---- */
typedef unsigned long pgoff_t;
static inline bool mem_is_zero(const void *s, size_t n)
{
	const unsigned char *p = s;
	while (n--)
		if (*p++)
			return false;
	return true;
}
#define strtomem_pad(dest, src, pad) do { \
	size_t _n = strnlen(src, sizeof(dest)); \
	memcpy(dest, src, _n); \
	memset((char *)(dest) + _n, pad, sizeof(dest) - _n); } while (0)
#define list_for_each_entry_from_reverse(pos, head, member) \
	for (; &pos->member != (head); pos = list_prev_entry(pos, member))
struct fwnode_handle { struct fwnode_handle *secondary; const void *ops; };
#define fwnode_handle_put(f) do { } while (0)
#define fwnode_handle_get(f) (f)
#define lock_acquire_shared_recursive(...) do { } while (0) /* args may name lockdep-only maps */
#define lock_release(...) do { } while (0)
#define _RET_IP_ ((unsigned long)__builtin_return_address(0))
struct of_device_id { char name[32]; char type[32]; char compatible[128]; const void *data; };
#ifndef KHZ2PICOS
#define KHZ2PICOS(a) (1000000000UL / (a))
#endif
#define CEC_PHYS_ADDR_INVALID 0xffff
#define NR_PAGE_ORDERS 11
#define MAX_PAGE_ORDER 10
struct of_endpoint { unsigned int port; unsigned int id; const struct device_node *local_node; };
#define vga_switcheroo_lock_ddc(pdev) do { } while (0)
#define vga_switcheroo_unlock_ddc(pdev) do { } while (0)
#define vga_switcheroo_handler_flags() 0
