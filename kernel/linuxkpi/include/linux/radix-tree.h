/* LinuxKPI: radix trees, as a thin layer over the xarray (as in Linux,
 * where the radix tree API is implemented by the xarray). */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/xarray.h>

struct radix_tree_root { struct xarray xa; };
struct radix_tree_iter { unsigned long index; };
#define RADIX_TREE_INIT(name, mask) { .xa = { .flags = 0 } }
#define RADIX_TREE(name, mask) struct radix_tree_root name = RADIX_TREE_INIT(name, mask)
#define INIT_RADIX_TREE(root, mask) xa_init(&(root)->xa)

static inline int radix_tree_insert(struct radix_tree_root *root, unsigned long index, void *item)
{
	if (xa_load(&root->xa, index))
		return -EEXIST;
	return xa_err(xa_store(&root->xa, index, item, GFP_KERNEL));
}
static inline void *radix_tree_lookup(const struct radix_tree_root *root, unsigned long index)
{
	return xa_load((struct xarray *)&root->xa, index);
}
static inline void *radix_tree_delete(struct radix_tree_root *root, unsigned long index)
{
	return xa_erase(&root->xa, index);
}
/* The next occupied slot at or after iter->index (NULL at the end). */
static inline void **lkpi_radix_next_slot(struct radix_tree_root *root, struct radix_tree_iter *iter)
{
	for (; iter->index < root->xa.size; iter->index++) {
		void *e = root->xa.slots[iter->index];
		if (e && e != (void *)1)
			return &root->xa.slots[iter->index];
	}
	return NULL;
}
#define radix_tree_for_each_slot(slot, root, iter, start) \
	for ((iter)->index = (start); ((slot) = lkpi_radix_next_slot(root, iter)) != NULL; (iter)->index++)
#define radix_tree_iter_delete(root, iter, slot) ((void)xa_erase(&(root)->xa, (iter)->index))
#define radix_tree_deref_slot(slot) (*(slot))
#define radix_tree_empty(root) (xa_empty(&(root)->xa))
