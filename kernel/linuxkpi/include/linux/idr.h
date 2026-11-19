/* LinuxKPI: ID allocators (idr: id -> pointer map, ida: id bitmap).
 * A small sorted array; plenty for the handful of ids drivers use. */
#ifndef _LKPI_LINUX_IDR_H
#define _LKPI_LINUX_IDR_H

#include <linux/lkpi_all.h>

LKPI_EXTERN_C_BEGIN

struct lkpi_idr_entry {
    unsigned long id;
    void* ptr;
};
struct idr {
    struct lkpi_idr_entry* entries;
    unsigned int count, capacity;
    unsigned int idr_base;
    unsigned int idr_next;
    spinlock_t lock;
};
#define IDR_INIT(name) {NULL, 0, 0, 0, 0, __SPIN_LOCK_UNLOCKED(name.lock)}
#define DEFINE_IDR(name) struct idr name = IDR_INIT(name)
static inline void idr_init(struct idr* idr) { memset(idr, 0, sizeof(*idr)); spin_lock_init(&idr->lock); }
static inline void idr_init_base(struct idr* idr, int base) { idr_init(idr); idr->idr_base = base; }
int idr_alloc(struct idr* idr, void* ptr, int start, int end, gfp_t gfp);
int idr_alloc_cyclic(struct idr* idr, void* ptr, int start, int end, gfp_t gfp);
void* idr_find(const struct idr* idr, unsigned long id);
void* idr_remove(struct idr* idr, unsigned long id);
void* idr_replace(struct idr* idr, void* ptr, unsigned long id);
void* idr_get_next(struct idr* idr, int* nextid);
void* idr_get_next_ul(struct idr* idr, unsigned long* nextid);
int idr_for_each(const struct idr* idr, int (*fn)(int id, void* p, void* data), void* data);
void idr_destroy(struct idr* idr);
static inline bool idr_is_empty(const struct idr* idr) { return idr->count == 0; }
static inline void idr_preload(gfp_t gfp) {}
static inline void idr_preload_end(void) {}
#define idr_for_each_entry(idr, entry, id) \
    for (id = 0; ((entry) = idr_get_next(idr, &(id))) != NULL; id += 1U)
#define idr_for_each_entry_ul(idr, entry, tmp, id) \
    for (tmp = 0, id = 0; ((entry) = idr_get_next_ul(idr, &(id))) != NULL; id += 1UL)

struct ida {
    struct idr idr;
};
#define IDA_INIT(name) {IDR_INIT(name.idr)}
#define DEFINE_IDA(name) struct ida name = IDA_INIT(name)
static inline void ida_init(struct ida* ida) { idr_init(&ida->idr); }
int ida_alloc_range(struct ida* ida, unsigned int min, unsigned int max, gfp_t gfp);
static inline int ida_alloc(struct ida* ida, gfp_t gfp) { return ida_alloc_range(ida, 0, ~0U >> 1, gfp); }
static inline int ida_alloc_min(struct ida* ida, unsigned int min, gfp_t gfp) { return ida_alloc_range(ida, min, ~0U >> 1, gfp); }
static inline int ida_alloc_max(struct ida* ida, unsigned int max, gfp_t gfp) { return ida_alloc_range(ida, 0, max, gfp); }
void ida_free(struct ida* ida, unsigned int id);
static inline void ida_destroy(struct ida* ida) { idr_destroy(&ida->idr); }
#define ida_simple_get(ida, start, end, gfp) ida_alloc_range(ida, start, (end) - 1, gfp)
#define ida_simple_remove(ida, id) ida_free(ida, id)

LKPI_EXTERN_C_END

#endif
