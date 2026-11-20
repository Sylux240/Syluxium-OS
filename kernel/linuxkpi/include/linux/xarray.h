/* LinuxKPI: xarray - a small growable array of pointers indexed by an
 * unsigned long (enough for DRM's minors and similar small id spaces). */
#pragma once
#include <linux/lkpi_all.h>

struct xarray {
	spinlock_t xa_lock;
	void **slots;
	unsigned long size;
	unsigned int flags;
};
struct xa_limit { u32 max, min; };
#define XA_LIMIT(_min, _max) ((struct xa_limit){ .min = (_min), .max = (_max) })
#define xa_limit_32b XA_LIMIT(0, UINT_MAX)
#define xa_limit_31b XA_LIMIT(0, INT_MAX)
#define XA_FLAGS_ALLOC 0x1
#define XA_FLAGS_ALLOC1 0x3
#define XARRAY_INIT(name, fl) { .xa_lock = __SPIN_LOCK_UNLOCKED(name.xa_lock), .slots = NULL, .size = 0, .flags = (fl) }
#define DEFINE_XARRAY_FLAGS(name, fl) struct xarray name = XARRAY_INIT(name, fl)
#define DEFINE_XARRAY(name) DEFINE_XARRAY_FLAGS(name, 0)
#define DEFINE_XARRAY_ALLOC(name) DEFINE_XARRAY_FLAGS(name, XA_FLAGS_ALLOC)
#define DEFINE_XARRAY_ALLOC1(name) DEFINE_XARRAY_FLAGS(name, XA_FLAGS_ALLOC1)
static inline void xa_init_flags(struct xarray *xa, unsigned int flags)
{
	spin_lock_init(&xa->xa_lock);
	xa->slots = NULL;
	xa->size = 0;
	xa->flags = flags;
}
#define xa_init(xa) xa_init_flags(xa, 0)
#define xa_lock(xa) spin_lock(&(xa)->xa_lock)
#define xa_unlock(xa) spin_unlock(&(xa)->xa_lock)
#define xa_is_err(entry) false
#define xa_err(entry) 0
void *xa_load(struct xarray *xa, unsigned long index);
void *xa_store(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp);
void *xa_erase(struct xarray *xa, unsigned long index);
int xa_alloc(struct xarray *xa, u32 *id, void *entry, struct xa_limit limit, gfp_t gfp);
bool xa_empty(const struct xarray *xa);
void xa_destroy(struct xarray *xa);
#define __xa_store xa_store
#define __xa_erase xa_erase
#define xa_for_each(xa, index, entry) \
	for ((index) = 0; (index) < (xa)->size; (index)++) \
		if (((entry) = (xa)->slots[index]) != NULL)
