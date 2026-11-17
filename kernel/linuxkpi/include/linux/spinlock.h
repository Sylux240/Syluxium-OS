#ifndef _LKPI_LINUX_SPINLOCK_H
#define _LKPI_LINUX_SPINLOCK_H

#include <linux/kernel.h>

LKPI_EXTERN_C_BEGIN

/* Spinlocks map onto Syluxium's interrupt-disabling spinlock, so the plain,
 * _bh and _irqsave variants are all safe against interrupt handlers. A
 * zero-filled lock is a valid unlocked lock (DEFINE_SPINLOCK works). */
typedef struct spinlock {
    unsigned long opaque[2];
} spinlock_t;
typedef spinlock_t raw_spinlock_t;

/* rwlocks: a plain spinlock (correct, just less concurrent). */
typedef spinlock_t rwlock_t;

#define __SPIN_LOCK_UNLOCKED(name) {{0, 0}}
#define DEFINE_SPINLOCK(name) spinlock_t name = __SPIN_LOCK_UNLOCKED(name)
#define DEFINE_RWLOCK(name) DEFINE_SPINLOCK(name)

void spin_lock_init(spinlock_t* l);
void lkpi_spin_lock(spinlock_t* l);
void lkpi_spin_unlock(spinlock_t* l);
int lkpi_spin_trylock(spinlock_t* l);

#define spin_lock(l) lkpi_spin_lock(l)
#define spin_unlock(l) lkpi_spin_unlock(l)
#define spin_trylock(l) lkpi_spin_trylock(l)
#define spin_lock_bh(l) lkpi_spin_lock(l)
#define spin_unlock_bh(l) lkpi_spin_unlock(l)
#define spin_lock_irq(l) lkpi_spin_lock(l)
#define spin_unlock_irq(l) lkpi_spin_unlock(l)
#define spin_lock_irqsave(l, flags) do { (flags) = 0; lkpi_spin_lock(l); } while (0)
#define spin_unlock_irqrestore(l, flags) do { (void)(flags); lkpi_spin_unlock(l); } while (0)
#define spin_lock_nested(l, sub) lkpi_spin_lock(l)
#define raw_spin_lock_init spin_lock_init
#define raw_spin_lock spin_lock
#define raw_spin_unlock spin_unlock
#define raw_spin_lock_irqsave spin_lock_irqsave
#define raw_spin_unlock_irqrestore spin_unlock_irqrestore
#define rwlock_init spin_lock_init
#define read_lock(l) lkpi_spin_lock(l)
#define read_unlock(l) lkpi_spin_unlock(l)
#define write_lock(l) lkpi_spin_lock(l)
#define write_unlock(l) lkpi_spin_unlock(l)
#define read_lock_bh read_lock
#define read_unlock_bh read_unlock
#define write_lock_bh write_lock
#define write_unlock_bh write_unlock
#define write_lock_irqsave(l, f) spin_lock_irqsave(l, f)
#define write_unlock_irqrestore(l, f) spin_unlock_irqrestore(l, f)
#define assert_spin_locked(l) do { (void)(l); } while (0)

/* Bottom halves / local interrupts: our spinlocks already disable interrupts. */
#define local_bh_disable() do { } while (0)
#define local_bh_enable() do { } while (0)
void lkpi_local_irq_disable(void);
void lkpi_local_irq_enable(void);
unsigned long lkpi_local_irq_save(void);
void lkpi_local_irq_restore(unsigned long f);
#define local_irq_disable() lkpi_local_irq_disable()
#define local_irq_enable() lkpi_local_irq_enable()
#define local_irq_save(f) do { (f) = lkpi_local_irq_save(); } while (0)
#define local_irq_restore(f) lkpi_local_irq_restore(f)

/* RCU: readers are plain sections; synchronize_rcu waits a scheduler tick on every CPU. */
#define rcu_read_lock() do { } while (0)
#define rcu_read_unlock() do { } while (0)
#define rcu_dereference(p) READ_ONCE(p)
#define rcu_dereference_protected(p, c) (p)
#define rcu_access_pointer(p) READ_ONCE(p)
#define rcu_assign_pointer(p, v) smp_store_release(&(p), (v))
#define RCU_INIT_POINTER(p, v) ((p) = (v))
void synchronize_rcu(void);
#define synchronize_net() synchronize_rcu()

LKPI_EXTERN_C_END

#endif
