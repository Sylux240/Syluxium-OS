#ifndef _LKPI_LINUX_WAIT_H
#define _LKPI_LINUX_WAIT_H

#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>

LKPI_EXTERN_C_BEGIN

/* A wait queue: a Syluxium WaitQueue plus a change counter. wait_event*()
 * samples the counter, tests the condition, and sleeps until the counter
 * moves (any wake_up) or the deadline passes, so no wakeup is ever lost. */
typedef struct wait_queue_head {
    unsigned long opaque[6];
    volatile unsigned int seq;
    spinlock_t lock; /* drivers sometimes take it directly */
} wait_queue_head_t;

#define __WAIT_QUEUE_HEAD_INITIALIZER(name) {{0}, 0, {{0, 0}}}
#define DECLARE_WAIT_QUEUE_HEAD(name) wait_queue_head_t name = __WAIT_QUEUE_HEAD_INITIALIZER(name)

void init_waitqueue_head(wait_queue_head_t* wq);
void wake_up(wait_queue_head_t* wq);
#define wake_up_all(wq) wake_up(wq)
#define wake_up_interruptible(wq) wake_up(wq)
#define wake_up_interruptible_all(wq) wake_up(wq)
#define wake_up_locked(wq) wake_up(wq)

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
