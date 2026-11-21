/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: interrupt domains - drivers' private IRQ chips (amdgpu's ACP
 * audio) are not mapped. */
#include <linux/irq.h>
struct irq_domain;
struct irq_domain_ops { int (*map)(struct irq_domain *d, unsigned int virq, irq_hw_number_t hw); void (*unmap)(struct irq_domain *d, unsigned int virq); };
#define irq_domain_add_linear(node, size, ops, data) ((struct irq_domain *)NULL)
#define irq_domain_remove(d) do { } while (0)
#define irq_create_mapping(d, hw) 0U
#define irq_find_mapping(d, hw) 0U
#define irq_dispose_mapping(irq) do { } while (0)
#define generic_handle_domain_irq(d, hw) 0
#define irq_set_chip_and_handler(irq, chip, h) do { } while (0)
#define handle_level_irq NULL
#define irq_set_nested_thread(irq, n) do { } while (0)
