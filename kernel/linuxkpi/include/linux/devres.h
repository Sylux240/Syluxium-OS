/* LinuxKPI: managed device resources (glue/devmodel.c). */
#pragma once
#include <linux/lkpi_all.h>

LKPI_EXTERN_C_BEGIN

typedef void (*dr_release_t)(struct device *dev, void *res);
typedef int (*dr_match_t)(struct device *dev, void *res, void *match_data);

void *__devres_alloc_node(dr_release_t release, size_t size, gfp_t gfp, int nid, const char *name);
#define devres_alloc(release, size, gfp) __devres_alloc_node(release, size, gfp, -1, #release)
#define devres_alloc_node(release, size, gfp, nid) __devres_alloc_node(release, size, gfp, nid, #release)
void devres_add(struct device *dev, void *res);
void devres_free(void *res);
void *devres_find(struct device *dev, dr_release_t release, dr_match_t match, void *match_data);
void *devres_remove(struct device *dev, dr_release_t release, dr_match_t match, void *match_data);
int devres_destroy(struct device *dev, dr_release_t release, dr_match_t match, void *match_data);
int devres_release(struct device *dev, dr_release_t release, dr_match_t match, void *match_data);
void devres_release_all(struct device *dev);

LKPI_EXTERN_C_END
