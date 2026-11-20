/* LinuxKPI: irq_work - callbacks deferred out of interrupt context. LinuxKPI
 * code never runs in hard IRQ context, so they run on the system workqueue. */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/workqueue.h>

struct irq_work {
	struct work_struct work;
	void (*func)(struct irq_work *);
};

static inline void lkpi_irq_work_fn(struct work_struct *w)
{
	struct irq_work *iw = container_of(w, struct irq_work, work);
	iw->func(iw);
}
static inline void init_irq_work(struct irq_work *w, void (*func)(struct irq_work *))
{
	INIT_WORK(&w->work, lkpi_irq_work_fn);
	w->func = func;
}
#define IRQ_WORK_INIT(f) { .func = (f) }
static inline bool irq_work_queue(struct irq_work *w)
{
	if (!w->work.func)
		INIT_WORK(&w->work, lkpi_irq_work_fn);
	return queue_work(system_wq, &w->work);
}
static inline void irq_work_sync(struct irq_work *w) { flush_work(&w->work); }
