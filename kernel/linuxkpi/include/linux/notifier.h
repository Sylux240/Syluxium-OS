/* LinuxKPI: notifier chains (all variants serialized by one mutex/spinlock
 * per chain; callbacks run in the caller's context). */
#ifndef _LKPI_LINUX_NOTIFIER_H
#define _LKPI_LINUX_NOTIFIER_H

#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>

LKPI_EXTERN_C_BEGIN

struct notifier_block;
typedef int (*notifier_fn_t)(struct notifier_block* nb, unsigned long action, void* data);

struct notifier_block {
    notifier_fn_t notifier_call;
    struct notifier_block* next;
    int priority;
};

struct raw_notifier_head { struct notifier_block* head; };
struct atomic_notifier_head { spinlock_t lock; struct notifier_block* head; };
struct blocking_notifier_head { struct mutex mutex; struct notifier_block* head; };

#define RAW_NOTIFIER_HEAD(name) struct raw_notifier_head name = {NULL}
#define ATOMIC_NOTIFIER_HEAD(name) struct atomic_notifier_head name = {__SPIN_LOCK_UNLOCKED(name.lock), NULL}
#define BLOCKING_NOTIFIER_HEAD(name) struct blocking_notifier_head name = {__MUTEX_INITIALIZER(name.mutex), NULL}
#define RAW_INIT_NOTIFIER_HEAD(name) do { (name)->head = NULL; } while (0)
#define ATOMIC_INIT_NOTIFIER_HEAD(name) do { spin_lock_init(&(name)->lock); (name)->head = NULL; } while (0)
#define BLOCKING_INIT_NOTIFIER_HEAD(name) do { mutex_init(&(name)->mutex); (name)->head = NULL; } while (0)

#define NETLINK_URELEASE 0x0001
#define NOTIFY_DONE 0x0000
#define NOTIFY_OK 0x0001
#define NOTIFY_STOP_MASK 0x8000
#define NOTIFY_BAD (NOTIFY_STOP_MASK | 0x0002)
#define NOTIFY_STOP (NOTIFY_OK | NOTIFY_STOP_MASK)
static inline int notifier_from_errno(int err) { return err ? (NOTIFY_STOP_MASK | (NOTIFY_OK - err)) : NOTIFY_OK; }
static inline int notifier_to_errno(int ret) { ret &= ~NOTIFY_STOP_MASK; return ret > NOTIFY_OK ? NOTIFY_OK - ret : 0; }

int lkpi_notifier_chain_register(struct notifier_block** head, struct notifier_block* n);
int lkpi_notifier_chain_unregister(struct notifier_block** head, struct notifier_block* n);
int lkpi_notifier_call_chain(struct notifier_block** head, unsigned long val, void* v);

#define raw_notifier_chain_register(nh, n) lkpi_notifier_chain_register(&(nh)->head, n)
#define raw_notifier_chain_unregister(nh, n) lkpi_notifier_chain_unregister(&(nh)->head, n)
#define raw_notifier_call_chain(nh, val, v) lkpi_notifier_call_chain(&(nh)->head, val, v)
#define atomic_notifier_chain_register(nh, n) lkpi_notifier_chain_register(&(nh)->head, n)
#define atomic_notifier_chain_unregister(nh, n) lkpi_notifier_chain_unregister(&(nh)->head, n)
#define atomic_notifier_call_chain(nh, val, v) lkpi_notifier_call_chain(&(nh)->head, val, v)
#define blocking_notifier_chain_register(nh, n) ({ int __r; mutex_lock(&(nh)->mutex); __r = lkpi_notifier_chain_register(&(nh)->head, n); mutex_unlock(&(nh)->mutex); __r; })
#define blocking_notifier_chain_unregister(nh, n) ({ int __r; mutex_lock(&(nh)->mutex); __r = lkpi_notifier_chain_unregister(&(nh)->head, n); mutex_unlock(&(nh)->mutex); __r; })
#define blocking_notifier_call_chain(nh, val, v) ({ int __r; mutex_lock(&(nh)->mutex); __r = lkpi_notifier_call_chain(&(nh)->head, val, v); mutex_unlock(&(nh)->mutex); __r; })

LKPI_EXTERN_C_END

#endif
