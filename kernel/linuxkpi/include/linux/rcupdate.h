/* LinuxKPI: RCU. Readers disable preemption (like Linux's non-preemptible
 * RCU); synchronize_rcu() waits until every CPU has passed through the
 * scheduler; call_rcu()/kfree_rcu() callbacks run on the "lkpi-rcu" thread
 * after a grace period. */
#ifndef _LKPI_LINUX_RCUPDATE_H
#define _LKPI_LINUX_RCUPDATE_H

#include <linux/compiler.h>
#include <linux/kernel.h>

LKPI_EXTERN_C_BEGIN

struct callback_head {
    struct callback_head* next;
    void (*func)(struct callback_head* head);
};
#define rcu_head callback_head
typedef void (*rcu_callback_t)(struct rcu_head* head);

void lkpi_rcu_read_lock(void);
void lkpi_rcu_read_unlock(void);
void synchronize_rcu(void);
void call_rcu(struct rcu_head* head, rcu_callback_t func);
void rcu_barrier(void);
int lkpi_preempt_count(void);

#define rcu_read_lock() lkpi_rcu_read_lock()
#define rcu_read_unlock() lkpi_rcu_read_unlock()
#define rcu_read_lock_bh() lkpi_rcu_read_lock()
#define rcu_read_unlock_bh() lkpi_rcu_read_unlock()
#define rcu_read_lock_sched() lkpi_rcu_read_lock()
#define rcu_read_unlock_sched() lkpi_rcu_read_unlock()
#define rcu_read_lock_held() 1
#define rcu_read_lock_bh_held() 1
#define rcu_read_lock_any_held() 1
#define rcu_lockdep_assert(c, s) do { } while (0)
#define RCU_LOCKDEP_WARN(c, s) do { } while (0)
#define synchronize_rcu_expedited() synchronize_rcu()
#define synchronize_net() synchronize_rcu()
#define synchronize_rcu_bh() synchronize_rcu()
#define synchronize_sched() synchronize_rcu()
#define call_rcu_hurry call_rcu
#define cond_synchronize_rcu(x) synchronize_rcu()
#define rcu_head_init(h) do { } while (0)

#define rcu_dereference_raw(p) READ_ONCE(p)
#define rcu_dereference(p) READ_ONCE(p)
#define rcu_dereference_bh(p) READ_ONCE(p)
#define rcu_dereference_sched(p) READ_ONCE(p)
#define rcu_dereference_check(p, c) READ_ONCE(p)
#define rcu_dereference_bh_check(p, c) READ_ONCE(p)
#define rcu_dereference_protected(p, c) (p)
#define rcu_dereference_raw_check(p) READ_ONCE(p)
#define rcu_access_pointer(p) READ_ONCE(p)
#define rcu_assign_pointer(p, v) do { __typeof__(p) __v = (v); __atomic_store_n(&(p), __v, __ATOMIC_RELEASE); } while (0)
#define RCU_INIT_POINTER(p, v) do { (p) = (v); } while (0)
#define RCU_INITIALIZER(v) (v)
#define rcu_replace_pointer(rcu_ptr, ptr, c) ({ __typeof__(ptr) __tmp = rcu_dereference_protected((rcu_ptr), (c)); \
    rcu_assign_pointer((rcu_ptr), (ptr)); __tmp; })
#define rcu_pointer_handoff(p) (p)
#define unrcu_pointer(p) (p)

/* kfree_rcu: the callback "pointer" is the rcu_head's offset in the object. */
void lkpi_kfree_rcu(struct rcu_head* head, unsigned long offset);
#define kfree_rcu(ptr, rhf) do { __typeof__(ptr) ___p = (ptr); if (___p) \
    lkpi_kfree_rcu(&___p->rhf, (unsigned long)((char*)&___p->rhf - (char*)___p)); } while (0)
#define kvfree_rcu kfree_rcu
#define kfree_rcu_mightsleep(ptr) do { synchronize_rcu(); kfree(ptr); } while (0)

LKPI_EXTERN_C_END

#endif
