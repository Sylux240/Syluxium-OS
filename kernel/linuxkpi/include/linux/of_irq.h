/* LinuxKPI: interrupts described by a device tree (none on x86 PCs). */
#pragma once
#include <linux/lkpi_all.h>
#define MAX_PHANDLE_ARGS 16
struct device_node;
struct of_phandle_args {
    struct device_node *np;
    int args_count;
    uint32_t args[MAX_PHANDLE_ARGS];
};
static inline int of_irq_parse_one(struct device_node *device, int index, struct of_phandle_args *out_irq) { return -EINVAL; }
static inline int of_irq_parse_raw(const __be32 *addr, struct of_phandle_args *out_irq) { return -EINVAL; }
static inline unsigned int irq_create_of_mapping(struct of_phandle_args *irq_data) { return 0; }
#ifndef for_each_child_of_node
#define for_each_child_of_node(parent, child) for ((child) = NULL; (child) != NULL;)
#endif
