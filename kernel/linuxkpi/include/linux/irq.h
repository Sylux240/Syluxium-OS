/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/interrupt.h>
struct irq_chip { const char *name; void (*irq_mask)(void *); void (*irq_unmask)(void *); };
#define irq_set_chip_and_handler_name(irq, chip, h, n) do { } while (0)
#define irq_set_chip_data(irq, d) 0
#define irq_alloc_desc(node) (-ENOSYS)
#define irq_free_desc(irq) do { } while (0)
#define handle_simple_irq NULL
#define generic_handle_irq(irq) 0
