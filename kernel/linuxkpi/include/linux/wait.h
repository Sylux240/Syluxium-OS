#ifndef _LKPI_LINUX_WAIT_H
#define _LKPI_LINUX_WAIT_H

#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/list.h>

LKPI_EXTERN_C_BEGIN

/* A wait queue: a Syluxium WaitQueue plus a change counter. wait_event*()
 * samples the counter, tests the condition, and sleeps until the counter
 * moves (any wake_up) or the deadline passes, so no wakeup is ever lost. */
typedef struct wait_queue_head {
    unsigned long opaque[6];
    volatile unsigned int seq;
    spinlock_t lock; /* drivers sometimes take it directly */
    struct list_head head; /* Linux-style entries (wait_queue_entry); NULL next = empty */
} wait_queue_head_t;

/* Linux wait queue entries: callbacks run by wake_up() (dma-fence and
 * i915_sw_fence chains, prepare_to_wait() sleepers). C only ("private"). */
#ifndef __cplusplus
struct wait_queue_entry;
typedef int (*wait_queue_func_t)(struct wait_queue_entry *wq_entry, unsigned mode, int flags, void *key);
typedef struct wait_queue_entry {
    unsigned int flags;
    void *private;
    wait_queue_func_t func;
    struct list_head entry;
} wait_queue_entry_t;
#define WQ_FLAG_EXCLUSIVE 0x01
#define WQ_FLAG_WOKEN 0x02
int default_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int flags, void *key);
int autoremove_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int sync, void *key);
int woken_wake_function(struct wait_queue_entry *wq_entry, unsigned mode, int sync, void *key);
#define DEFINE_WAIT_FUNC(name, function) struct wait_queue_entry name = { .private = lkpi_current(), .func = function, .entry = LIST_HEAD_INIT((name).entry) }
#define DEFINE_WAIT(name) DEFINE_WAIT_FUNC(name, autoremove_wake_function)
#define DECLARE_WAITQUEUE(name, tsk) struct wait_queue_entry name = { .private = tsk, .func = default_wake_function, .entry = { NULL, NULL } }
#define init_wait(wq) do { (wq)->private = lkpi_current(); (wq)->func = autoremove_wake_function; INIT_LIST_HEAD(&(wq)->entry); (wq)->flags = 0; } while (0)
static inline void init_waitqueue_entry(struct wait_queue_entry *e, struct task_struct *p)
{ e->flags = 0; e->private = p; e->func = default_wake_function; INIT_LIST_HEAD(&e->entry); }
static inline void init_waitqueue_func_entry(struct wait_queue_entry *e, wait_queue_func_t func)
{ e->flags = 0; e->private = NULL; e->func = func; INIT_LIST_HEAD(&e->entry); }
void lkpi_wq_head_ready(struct wait_queue_head *wq);
void add_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e);
void add_wait_queue_exclusive(struct wait_queue_head *wq, struct wait_queue_entry *e);
void add_wait_queue_priority(struct wait_queue_head *wq, struct wait_queue_entry *e);
void remove_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e);
static inline void __add_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{ lkpi_wq_head_ready(wq); list_add(&e->entry, &wq->head); }
static inline void __add_wait_queue_entry_tail(struct wait_queue_head *wq, struct wait_queue_entry *e)
{ lkpi_wq_head_ready(wq); list_add_tail(&e->entry, &wq->head); }
static inline void __add_wait_queue_exclusive(struct wait_queue_head *wq, struct wait_queue_entry *e)
{ e->flags |= WQ_FLAG_EXCLUSIVE; __add_wait_queue(wq, e); }
static inline void __remove_wait_queue(struct wait_queue_head *wq, struct wait_queue_entry *e)
{ list_del(&e->entry); }
void prepare_to_wait(struct wait_queue_head *wq, struct wait_queue_entry *e, int state);
bool prepare_to_wait_exclusive(struct wait_queue_head *wq, struct wait_queue_entry *e, int state);
long prepare_to_wait_event(struct wait_queue_head *wq, struct wait_queue_entry *e, int state);
void finish_wait(struct wait_queue_head *wq, struct wait_queue_entry *e);
long wait_woken(struct wait_queue_entry *e, unsigned mode, long timeout);
/* Conservative: Syluxium-side sleepers aren't on the list, so always wake. */
#define waitqueue_active(wq) true
#define wq_has_sleeper(wq) true
void __wake_up(struct wait_queue_head *wq, unsigned int mode, int nr, void *key);
void __wake_up_locked(struct wait_queue_head *wq, unsigned int mode, int nr);
void __wake_up_locked_key(struct wait_queue_head *wq, unsigned int mode, void *key);
#define wake_up_nr(wq, nr) __wake_up(wq, TASK_NORMAL, nr, NULL)
#define wake_up_poll(wq, m) __wake_up(wq, TASK_NORMAL, 1, (void *)(uintptr_t)(m))
#define wake_up_interruptible_poll(wq, m) __wake_up(wq, TASK_INTERRUPTIBLE, 1, (void *)(uintptr_t)(m))
#define wake_up_locked_poll(wq, m) __wake_up_locked_key(wq, TASK_NORMAL, (void *)(uintptr_t)(m))
#define wake_up_all_locked(wq) __wake_up_locked(wq, TASK_NORMAL, 0)
#define __init_waitqueue_head(wq, name, key) init_waitqueue_head(wq)
#endif /* !__cplusplus */

#define __WAIT_QUEUE_HEAD_INITIALIZER(name) {{0}, 0, {{0, 0}}, { NULL, NULL }}
#define DECLARE_WAIT_QUEUE_HEAD(name) wait_queue_head_t name = __WAIT_QUEUE_HEAD_INITIALIZER(name)

void init_waitqueue_head(wait_queue_head_t* wq);
void wake_up(wait_queue_head_t* wq);
#define wake_up_all(wq) wake_up(wq)
#define wake_up_interruptible(wq) wake_up(wq)
#define wake_up_interruptible_all(wq) wake_up(wq)
#define wake_up_locked(wq) __wake_up_locked(wq, TASK_NORMAL, 1)

/* Sleep until `seq` changes, `deadline` (jiffies, 0 = none) passes or (if
 * interruptible) a signal arrives. Returns -ERESTARTSYS on a signal. */
int lkpi_wait_sleep(wait_queue_head_t* wq, unsigned int seen_seq, unsigned long deadline, int interruptible);

#define __lkpi_wait(wq, cond, timeout, intr) ({                                  \
    long __ret = (timeout);                                                       \
    unsigned long __end = (timeout) == MAX_SCHEDULE_TIMEOUT ? 0 : jiffies + (timeout); \
    for (;;) {                                                                    \
        unsigned int __seq = READ_ONCE((wq).seq);                                 \
        if (cond) { __ret = __end ? (long)max_t(long, 1, (long)(__end - jiffies)) : 1; break; } \
        if (__end && time_after_eq(jiffies, __end)) { __ret = 0; break; }        \
        if (lkpi_wait_sleep(&(wq), __seq, __end, (intr)) < 0) { __ret = -ERESTARTSYS; break; } \
    }                                                                             \
    __ret; })

#define wait_event(wq, cond) do { (void)__lkpi_wait(wq, cond, MAX_SCHEDULE_TIMEOUT, 0); } while (0)
#define wait_event_timeout(wq, cond, t) __lkpi_wait(wq, cond, (long)(t), 0)
#define wait_event_interruptible(wq, cond) ({ long __r = __lkpi_wait(wq, cond, MAX_SCHEDULE_TIMEOUT, 1); __r < 0 ? (int)__r : 0; })
#define wait_event_interruptible_timeout(wq, cond, t) __lkpi_wait(wq, cond, (long)(t), 1)
#define wait_event_killable(wq, cond) wait_event_interruptible(wq, cond)
#define wait_event_freezable(wq, cond) wait_event_interruptible(wq, cond)

LKPI_EXTERN_C_END

#endif
