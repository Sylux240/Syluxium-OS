/* LinuxKPI: irq_poll, the interrupt-mitigation poller of block drivers
 * (include/linux/irq_poll.h). Linux runs it in a softirq; here a work
 * item calls ->poll() until it completes (glue/irq_poll.c), like NAPI. */
#pragma once
#include <linux/lkpi_all.h>

struct irq_poll;
typedef int (irq_poll_fn)(struct irq_poll *, int);

struct irq_poll {
	unsigned long state;
	int weight;
	irq_poll_fn *poll;
	struct work_struct lkpi_work;
};

enum { IRQ_POLL_F_SCHED = 0, IRQ_POLL_F_DISABLE = 1 };

void irq_poll_init(struct irq_poll *iop, int weight, irq_poll_fn *poll_fn);
void irq_poll_sched(struct irq_poll *iop);
void irq_poll_complete(struct irq_poll *iop);
void irq_poll_disable(struct irq_poll *iop);
void irq_poll_enable(struct irq_poll *iop);
