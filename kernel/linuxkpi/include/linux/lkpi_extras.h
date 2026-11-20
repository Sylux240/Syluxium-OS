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
void wbinvd_on_all_cpus(void);
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
int sg_alloc_table(struct sg_table *t, unsigned int nents, gfp_t gfp);
void sg_free_table(struct sg_table *t);
