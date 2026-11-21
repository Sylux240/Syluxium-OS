/* LinuxKPI: the device tree. PCs have none: every lookup finds nothing
 * (the node handles drivers get, dev->of_node, are always NULL). */
#pragma once
#include <linux/lkpi_all.h>

struct property {
    char *name;
    int length;
    void *value;
    struct property *next;
};

static inline struct device_node *of_get_child_by_name(const struct device_node *node, const char *name) { return NULL; }
static inline struct device_node *of_get_next_child(const struct device_node *node, struct device_node *prev) { return NULL; }
static inline struct property *of_find_property(const struct device_node *np, const char *name, int *lenp) { return NULL; }
static inline bool of_property_read_bool(const struct device_node *np, const char *name) { return false; }
static inline bool of_device_is_available(const struct device_node *np) { return false; }
static inline const char *of_prop_next_string(struct property *prop, const char *cur) { return NULL; }
static inline const void *of_get_property(const struct device_node *np, const char *name, int *lenp) { return NULL; }
#ifndef for_each_child_of_node
#define for_each_child_of_node(parent, child) for ((child) = NULL; (child) != NULL;)
#endif
#define of_property_for_each_string(np, propname, prop, s) \
    for (prop = of_find_property(np, propname, NULL), s = of_prop_next_string(prop, NULL); s; \
         s = of_prop_next_string(prop, s))
