// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI: irq_poll on a work item (Linux: lib/irq_poll.c, a softirq).
 * irq_poll_sched() from the interrupt handler queues the work; the work
 * calls ->poll(weight) until the driver calls irq_poll_complete().
 */
#include <linux/lkpi_all.h>
#include <linux/irq_poll.h>

static void irq_poll_work(struct work_struct *w)
{
	struct irq_poll *iop = container_of(w, struct irq_poll, lkpi_work);
	int rounds = 0;
	while (test_bit(IRQ_POLL_F_SCHED, &iop->state) && !test_bit(IRQ_POLL_F_DISABLE, &iop->state)) {
		int done = iop->poll(iop, iop->weight);
		if (done < iop->weight)
			break; /* driver called irq_poll_complete() or will be interrupted again */
		if (++rounds >= 16) {
			/* Busy device: let others run, then go on. */
			queue_work(system_highpri_wq, &iop->lkpi_work);
			return;
		}
	}
}

void irq_poll_init(struct irq_poll *iop, int weight, irq_poll_fn *poll_fn)
{
	memset(iop, 0, sizeof(*iop));
	iop->weight = weight;
	iop->poll = poll_fn;
	INIT_WORK(&iop->lkpi_work, irq_poll_work);
}

void irq_poll_sched(struct irq_poll *iop)
{
	if (test_bit(IRQ_POLL_F_DISABLE, &iop->state))
		return;
	if (test_and_set_bit(IRQ_POLL_F_SCHED, &iop->state))
		return;
	queue_work(system_highpri_wq, &iop->lkpi_work);
}

void irq_poll_complete(struct irq_poll *iop)
{
	smp_mb__before_atomic();
	clear_bit(IRQ_POLL_F_SCHED, &iop->state);
}

void irq_poll_disable(struct irq_poll *iop)
{
	set_bit(IRQ_POLL_F_DISABLE, &iop->state);
	cancel_work_sync(&iop->lkpi_work);
	clear_bit(IRQ_POLL_F_SCHED, &iop->state);
}

void irq_poll_enable(struct irq_poll *iop)
{
	smp_mb__before_atomic();
	clear_bit(IRQ_POLL_F_DISABLE, &iop->state);
}
