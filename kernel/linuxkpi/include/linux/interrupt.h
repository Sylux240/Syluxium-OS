#ifndef _LKPI_LINUX_INTERRUPT_H
#define _LKPI_LINUX_INTERRUPT_H

#include <linux/kernel.h>
#include <linux/workqueue.h>

LKPI_EXTERN_C_BEGIN

typedef enum irqreturn {
    IRQ_NONE = 0,
    IRQ_HANDLED = 1,
    IRQ_WAKE_THREAD = 2,
} irqreturn_t;
typedef irqreturn_t (*irq_handler_t)(int irq, void* dev_id);
#define IRQ_RETVAL(x) ((x) ? IRQ_HANDLED : IRQ_NONE)

#define IRQF_SHARED 0x80
#define IRQF_ONESHOT 0x2000
#define IRQF_NO_SUSPEND 0x4000

/* `irq` numbers are interrupt vectors handed out by pci_alloc_irq_vectors /
 * pci_enable_device (MSI or MSI-X). The hard handler runs in interrupt
 * context; a threaded handler runs on a dedicated kernel thread. */
int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags, const char* name, void* dev_id);
int request_threaded_irq(unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn, unsigned long flags,
                         const char* name, void* dev_id);
void free_irq(unsigned int irq, void* dev_id);
#define devm_request_irq(dev, irq, h, f, n, id) request_irq(irq, h, f, n, id)
#define devm_request_threaded_irq(dev, irq, h, t, f, n, id) request_threaded_irq(irq, h, t, f, n, id)
void disable_irq(unsigned int irq);
void disable_irq_nosync(unsigned int irq);
void enable_irq(unsigned int irq);
void synchronize_irq(unsigned int irq);
#define in_interrupt() lkpi_in_interrupt()
#define in_irq() lkpi_in_interrupt()
#define in_atomic() 0
int lkpi_in_interrupt(void);

/* Tasklets: deferred to the system workqueue. */
struct tasklet_struct {
    struct work_struct work;
    void (*func)(unsigned long data);
    void (*callback)(struct tasklet_struct* t);
    unsigned long data;
    int disabled;
};
void tasklet_init(struct tasklet_struct* t, void (*func)(unsigned long), unsigned long data);
void tasklet_setup(struct tasklet_struct* t, void (*cb)(struct tasklet_struct*));
void tasklet_schedule(struct tasklet_struct* t);
#define tasklet_hi_schedule tasklet_schedule
void tasklet_kill(struct tasklet_struct* t);
#define tasklet_disable(t) ((t)->disabled = 1)
#define tasklet_enable(t) ((t)->disabled = 0)
#define from_tasklet(var, t, field) container_of(t, __typeof__(*var), field)

LKPI_EXTERN_C_END

#endif
