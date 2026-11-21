#ifndef _LKPI_LINUX_WORKQUEUE_H
#define _LKPI_LINUX_WORKQUEUE_H

#include <linux/kernel.h>
#include <linux/timer.h>

LKPI_EXTERN_C_BEGIN

struct work_struct;
typedef void (*work_func_t)(struct work_struct* w);

struct work_struct {
    work_func_t func;
    /* private */
    struct work_struct* lkpi_next;
    struct workqueue_struct* lkpi_wq;
    volatile int lkpi_state; /* bit 0: queued, bit 1: running */
};

struct delayed_work {
    struct work_struct work;
    struct timer_list timer;
    struct workqueue_struct* wq;
};

/* Each workqueue has its own worker thread (ordered execution). */
struct workqueue_struct;

extern struct workqueue_struct* system_wq;
extern struct workqueue_struct* system_highpri_wq;
extern struct workqueue_struct* system_long_wq;
extern struct workqueue_struct* system_unbound_wq;
extern struct workqueue_struct* system_freezable_wq;
extern struct workqueue_struct* system_power_efficient_wq;

void lkpi_init_work(struct work_struct* w, work_func_t fn);
void lkpi_init_delayed_work(struct delayed_work* d, work_func_t fn);
#define INIT_WORK(w, fn) lkpi_init_work(w, fn)
#define INIT_DELAYED_WORK(d, fn) lkpi_init_delayed_work(d, fn)
#define INIT_DEFERRABLE_WORK(d, fn) lkpi_init_delayed_work(d, fn)
#define DECLARE_WORK(name, fn) struct work_struct name = {fn, 0, 0, 0}
#define to_delayed_work(w) container_of(w, struct delayed_work, work)
void lkpi_delayed_work_timer(struct timer_list* t);
#define DECLARE_DELAYED_WORK(n, f) struct delayed_work n = { .work = { .func = (f) }, .timer = { .function = lkpi_delayed_work_timer } }
#define DECLARE_DEFERRABLE_WORK(n, f) DECLARE_DELAYED_WORK(n, f)
#define __DELAYED_WORK_INITIALIZER(n, f, tflags) { .work = { .func = (f) }, .timer = { .function = lkpi_delayed_work_timer } }
#define work_busy(w) work_pending(w)
#define current_work() ((struct work_struct*)0)
#define enable_and_queue_work(wq, w) queue_work(wq, w)
#define disable_work_sync(w) cancel_work_sync(w)
#define disable_delayed_work_sync(d) cancel_delayed_work_sync(d)
#define WQ_PERCPU 0
#define WQ_BH 0

struct workqueue_struct* alloc_workqueue(const char* fmt, unsigned int flags, int max_active, ...);
#define alloc_ordered_workqueue(fmt, flags, ...) alloc_workqueue(fmt, flags, 1, ##__VA_ARGS__)
#define create_singlethread_workqueue(name) alloc_workqueue("%s", 0, 1, name)
#define create_workqueue(name) alloc_workqueue("%s", 0, 1, name)
void destroy_workqueue(struct workqueue_struct* wq);
void flush_workqueue(struct workqueue_struct* wq);
#define drain_workqueue flush_workqueue
#define WQ_UNBOUND 0x2
#define WQ_FREEZABLE 0x4
#define WQ_MEM_RECLAIM 0x8
#define WQ_HIGHPRI 0x10
#define WQ_SYSFS 0x40

bool queue_work(struct workqueue_struct* wq, struct work_struct* w);
bool queue_delayed_work(struct workqueue_struct* wq, struct delayed_work* d, unsigned long delay);
bool mod_delayed_work(struct workqueue_struct* wq, struct delayed_work* d, unsigned long delay);
#define schedule_work(w) queue_work(system_wq, w)
#define schedule_delayed_work(d, delay) queue_delayed_work(system_wq, d, delay)
bool cancel_work_sync(struct work_struct* w);
bool cancel_work(struct work_struct* w);
bool cancel_delayed_work(struct delayed_work* d);
bool cancel_delayed_work_sync(struct delayed_work* d);
bool flush_work(struct work_struct* w);
bool flush_delayed_work(struct delayed_work* d);
bool work_pending(const struct work_struct* w);
#define delayed_work_pending(d) work_pending(&(d)->work)
#define flush_scheduled_work() flush_workqueue(system_wq)

LKPI_EXTERN_C_END

#endif
