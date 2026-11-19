/* LinuxKPI: resizable hash tables, simplified - a fixed number of buckets
 * (no resizing), RCU-safe lookups, writers serialized by a spinlock. The
 * API (rhashtable_* and the duplicate-key rhltable_*) is Linux's. */
#ifndef _LKPI_LINUX_RHASHTABLE_H
#define _LKPI_LINUX_RHASHTABLE_H

#include <linux/lkpi_all.h>
#include <linux/jhash.h>

LKPI_EXTERN_C_BEGIN

#define LKPI_RHT_BUCKETS 64

struct rhash_head {
    struct rhash_head __rcu* next;
};
struct rhlist_head {
    struct rhash_head rhead;
    struct rhlist_head __rcu* next;
};

struct rhashtable_compare_arg {
    struct rhashtable* ht;
    const void* key;
};
typedef u32 (*rht_hashfn_t)(const void* data, u32 len, u32 seed);
typedef u32 (*rht_obj_hashfn_t)(const void* data, u32 len, u32 seed);
typedef int (*rht_obj_cmpfn_t)(struct rhashtable_compare_arg* arg, const void* obj);

struct rhashtable_params {
    u16 nelem_hint;
    u16 key_len;
    u16 key_offset;
    u16 head_offset;
    unsigned int max_size;
    u16 min_size;
    bool automatic_shrinking;
    rht_hashfn_t hashfn;
    rht_obj_hashfn_t obj_hashfn;
    rht_obj_cmpfn_t obj_cmpfn;
};

struct rhashtable {
    struct rhash_head __rcu* buckets[LKPI_RHT_BUCKETS];
    struct rhashtable_params p;
    bool rhlist;
    atomic_t nelems;
    spinlock_t lock;
};
struct rhltable {
    struct rhashtable ht;
};

struct rhashtable_iter {
    struct rhashtable* ht;
    unsigned int bucket;
    struct rhash_head* p;
    struct rhlist_head* list;
    unsigned int skip;
};

int rhashtable_init(struct rhashtable* ht, const struct rhashtable_params* params);
int rhltable_init(struct rhltable* hlt, const struct rhashtable_params* params);
void rhashtable_destroy(struct rhashtable* ht);
void rhashtable_free_and_destroy(struct rhashtable* ht, void (*free_fn)(void* ptr, void* arg), void* arg);
static inline void rhltable_destroy(struct rhltable* hlt) { rhashtable_destroy(&hlt->ht); }
static inline void rhltable_free_and_destroy(struct rhltable* hlt, void (*free_fn)(void* ptr, void* arg), void* arg) {
    rhashtable_free_and_destroy(&hlt->ht, free_fn, arg);
}

/* Out-of-line core (glue/rhashtable.c). params may differ from ht->p only
 * in being a compile-time copy; ht->p is used. */
void* lkpi_rht_lookup(struct rhashtable* ht, const void* key);
struct rhlist_head* lkpi_rhl_lookup(struct rhltable* hlt, const void* key);
int lkpi_rht_insert(struct rhashtable* ht, struct rhash_head* obj, bool unique);
int lkpi_rht_remove(struct rhashtable* ht, struct rhash_head* obj);
int lkpi_rhl_insert(struct rhltable* hlt, struct rhlist_head* list);
int lkpi_rhl_remove(struct rhltable* hlt, struct rhlist_head* list);
void* lkpi_rht_lookup_get_insert(struct rhashtable* ht, const void* key, struct rhash_head* obj);

#define rht_obj(ht, he) ((void*)((char*)(he) - (ht)->p.head_offset))
#define rht_entry(tpos, pos, member) ({ tpos = container_of(pos, __typeof__(*tpos), member); 1; })

static inline void* rhashtable_lookup(struct rhashtable* ht, const void* key, const struct rhashtable_params params) {
    return lkpi_rht_lookup(ht, key);
}
static inline void* rhashtable_lookup_fast(struct rhashtable* ht, const void* key, const struct rhashtable_params params) {
    void* obj;
    rcu_read_lock();
    obj = lkpi_rht_lookup(ht, key);
    rcu_read_unlock();
    return obj;
}
static inline int rhashtable_insert_fast(struct rhashtable* ht, struct rhash_head* obj, const struct rhashtable_params params) {
    return lkpi_rht_insert(ht, obj, false);
}
static inline int rhashtable_lookup_insert_fast(struct rhashtable* ht, struct rhash_head* obj, const struct rhashtable_params params) {
    return lkpi_rht_insert(ht, obj, true);
}
static inline int rhashtable_lookup_insert_key(struct rhashtable* ht, const void* key, struct rhash_head* obj,
                                               const struct rhashtable_params params) {
    return lkpi_rht_insert(ht, obj, true);
}
static inline void* rhashtable_lookup_get_insert_fast(struct rhashtable* ht, struct rhash_head* obj,
                                                      const struct rhashtable_params params) {
    return lkpi_rht_lookup_get_insert(ht, (const char*)rht_obj(ht, obj) + ht->p.key_offset, obj);
}
static inline int rhashtable_remove_fast(struct rhashtable* ht, struct rhash_head* obj, const struct rhashtable_params params) {
    return lkpi_rht_remove(ht, obj);
}
static inline struct rhlist_head* rhltable_lookup(struct rhltable* hlt, const void* key, const struct rhashtable_params params) {
    return lkpi_rhl_lookup(hlt, key);
}
static inline int rhltable_insert(struct rhltable* hlt, struct rhlist_head* list, const struct rhashtable_params params) {
    return lkpi_rhl_insert(hlt, list);
}
static inline int rhltable_insert_key(struct rhltable* hlt, const void* key, struct rhlist_head* list,
                                      const struct rhashtable_params params) {
    return lkpi_rhl_insert(hlt, list);
}
static inline int rhltable_remove(struct rhltable* hlt, struct rhlist_head* list, const struct rhashtable_params params) {
    return lkpi_rhl_remove(hlt, list);
}

#define rhl_for_each_rcu(pos, list) for (pos = list; pos; pos = rcu_dereference_raw(pos->next))
#define rhl_for_each_entry_rcu(tpos, pos, list, member) \
    for (pos = list; pos && rht_entry(tpos, pos, member); pos = rcu_dereference_raw(pos->next))

/* Walking (used for dumps): bucket by bucket. */
void rhashtable_walk_enter(struct rhashtable* ht, struct rhashtable_iter* iter);
void rhashtable_walk_exit(struct rhashtable_iter* iter);
int rhashtable_walk_start_check(struct rhashtable_iter* iter);
static inline void rhashtable_walk_start(struct rhashtable_iter* iter) { (void)rhashtable_walk_start_check(iter); }
void* rhashtable_walk_next(struct rhashtable_iter* iter);
void rhashtable_walk_stop(struct rhashtable_iter* iter);
static inline void rhltable_walk_enter(struct rhltable* hlt, struct rhashtable_iter* iter) { rhashtable_walk_enter(&hlt->ht, iter); }

LKPI_EXTERN_C_END

#endif
