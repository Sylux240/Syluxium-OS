#ifndef _LKPI_LINUX_ATOMIC_H
#define _LKPI_LINUX_ATOMIC_H

#include <linux/kernel.h>

LKPI_EXTERN_C_BEGIN

#define ATOMIC_INIT(i) {(i)}
#define atomic_read(v) __atomic_load_n(&(v)->counter, __ATOMIC_RELAXED)
#define atomic_set(v, i) __atomic_store_n(&(v)->counter, (i), __ATOMIC_RELAXED)
#define atomic_add(i, v) ((void)__atomic_add_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_sub(i, v) ((void)__atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_inc(v) atomic_add(1, v)
#define atomic_dec(v) atomic_sub(1, v)
#define atomic_add_return(i, v) __atomic_add_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic_sub_return(i, v) __atomic_sub_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic_inc_return(v) atomic_add_return(1, v)
#define atomic_dec_return(v) atomic_sub_return(1, v)
#define atomic_fetch_add(i, v) __atomic_fetch_add(&(v)->counter, (i), __ATOMIC_SEQ_CST)
#define atomic_fetch_inc(v) atomic_fetch_add(1, v)
#define atomic_dec_and_test(v) (atomic_dec_return(v) == 0)
#define atomic_inc_and_test(v) (atomic_inc_return(v) == 0)
#define atomic_sub_and_test(i, v) (atomic_sub_return(i, v) == 0)
#define atomic_xchg(v, n) __atomic_exchange_n(&(v)->counter, (n), __ATOMIC_SEQ_CST)
#define atomic_or(i, v) ((void)__atomic_or_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
#define atomic_and(i, v) ((void)__atomic_and_fetch(&(v)->counter, (i), __ATOMIC_SEQ_CST))
static inline int atomic_cmpxchg(atomic_t* v, int old, int n) {
    __atomic_compare_exchange_n(&v->counter, &old, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return old;
}
static inline bool atomic_add_unless(atomic_t* v, int a, int u) {
    int c = atomic_read(v);
    while (c != u) { if (__atomic_compare_exchange_n(&v->counter, &c, c + a, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return true; }
    return false;
}
#define atomic_inc_not_zero(v) atomic_add_unless(v, 1, 0)
#define atomic64_read atomic_read
#define atomic64_set atomic_set
#define atomic64_add atomic_add
#define atomic64_sub atomic_sub
#define atomic64_inc atomic_inc
#define atomic64_dec atomic_dec
#define atomic64_inc_return atomic_inc_return
#define atomic64_add_return atomic_add_return
#define atomic_long_read atomic_read
#define atomic_long_set atomic_set
#define atomic_long_inc atomic_inc
#define atomic_long_dec atomic_dec
#define xchg(p, n) __atomic_exchange_n(p, n, __ATOMIC_SEQ_CST)
#define cmpxchg(p, o, n) ({ __typeof__(*(p)) __o = (o); __atomic_compare_exchange_n(p, &__o, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); __o; })

/* Reference counts. */
typedef struct { atomic_t refs; } refcount_t;
#define REFCOUNT_INIT(n) {ATOMIC_INIT(n)}
#define refcount_set(r, n) atomic_set(&(r)->refs, n)
#define refcount_read(r) atomic_read(&(r)->refs)
#define refcount_inc(r) atomic_inc(&(r)->refs)
#define refcount_dec(r) atomic_dec(&(r)->refs)
#define refcount_dec_and_test(r) atomic_dec_and_test(&(r)->refs)
#define refcount_inc_not_zero(r) atomic_inc_not_zero(&(r)->refs)
struct kref { refcount_t refcount; };
#define kref_init(k) refcount_set(&(k)->refcount, 1)
#define kref_get(k) refcount_inc(&(k)->refcount)
#define kref_read(k) refcount_read(&(k)->refcount)
static inline int kref_put(struct kref* k, void (*release)(struct kref*)) {
    if (refcount_dec_and_test(&k->refcount)) { release(k); return 1; }
    return 0;
}

LKPI_EXTERN_C_END

#endif
