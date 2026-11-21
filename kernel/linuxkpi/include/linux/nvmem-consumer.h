/* LinuxKPI: non-volatile memory cells (device-tree platforms only). */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/err.h>

struct nvmem_cell;
struct device_node;
static inline struct nvmem_cell *of_nvmem_cell_get(struct device_node *np, const char *id) { return ERR_PTR(-ENOENT); }
static inline struct nvmem_cell *nvmem_cell_get(struct device *dev, const char *id) { return ERR_PTR(-ENOENT); }
static inline void *nvmem_cell_read(struct nvmem_cell *cell, size_t *len) { return ERR_PTR(-ENOENT); }
static inline void nvmem_cell_put(struct nvmem_cell *cell) { }
static inline struct nvmem_cell *devm_nvmem_cell_get(struct device *dev, const char *id) { return ERR_PTR(-ENOENT); }
