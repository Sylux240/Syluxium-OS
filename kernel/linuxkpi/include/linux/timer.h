#ifndef _LKPI_LINUX_TIMER_H
#define _LKPI_LINUX_TIMER_H

#include <linux/jiffies.h>
#include <linux/kernel.h>

LKPI_EXTERN_C_BEGIN

/* Kernel timers. Callbacks run on the LinuxKPI timer thread (not in an
 * interrupt), which is a superset of what Linux softirq timers guarantee. */
struct timer_list {
    unsigned long expires;
    void (*function)(struct timer_list* t);
    unsigned int flags;
    /* private */
    struct timer_list* lkpi_next;
    int lkpi_pending;
};

#define TIMER_DEFERRABLE 0x1
#define TIMER_IRQSAFE 0x2

void timer_setup(struct timer_list* t, void (*fn)(struct timer_list*), unsigned int flags);
void add_timer(struct timer_list* t);
int mod_timer(struct timer_list* t, unsigned long expires);
int del_timer(struct timer_list* t);
int del_timer_sync(struct timer_list* t);
int timer_pending(const struct timer_list* t);
#define timer_delete del_timer
#define timer_delete_sync del_timer_sync
#define timer_shutdown_sync del_timer_sync
#define from_timer(var, t, field) container_of(t, __typeof__(*var), field)
#define DEFINE_TIMER(name, fn) struct timer_list name = {0, fn, 0, 0, 0}

LKPI_EXTERN_C_END

#endif
