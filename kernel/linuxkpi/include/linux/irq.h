/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/interrupt.h>
struct irq_data { unsigned int irq; void *chip_data; };
struct irq_chip { const char *name; void (*irq_mask)(struct irq_data *); void (*irq_unmask)(struct irq_data *); };
#define irq_data_get_irq_chip_data(d) ((d)->chip_data)
#define irq_set_chip_and_handler_name(irq, chip, h, n) do { } while (0)
#define irq_set_chip_data(irq, d) 0
#define irq_alloc_desc(node) (-ENOSYS)
#define irq_free_desc(irq) do { } while (0)
#define handle_simple_irq NULL
#define generic_handle_irq(irq) 0
