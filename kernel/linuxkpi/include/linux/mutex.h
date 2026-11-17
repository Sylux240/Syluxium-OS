#ifndef _LKPI_LINUX_MUTEX_H
#define _LKPI_LINUX_MUTEX_H

#include <linux/kernel.h>

LKPI_EXTERN_C_BEGIN

/* Sleeping mutex (Syluxium sched::Mutex underneath). Zero-filled = unlocked. */
struct mutex {
    unsigned long opaque[6];
};

#define __MUTEX_INITIALIZER(name) {{0}}
#define DEFINE_MUTEX(name) struct mutex name = __MUTEX_INITIALIZER(name)

void mutex_init(struct mutex* m);
void mutex_lock(struct mutex* m);
int mutex_lock_interruptible(struct mutex* m);
int mutex_trylock(struct mutex* m);
void mutex_unlock(struct mutex* m);
int mutex_is_locked(struct mutex* m);
#define mutex_destroy(m) do { (void)(m); } while (0)
#define mutex_lock_nested(m, s) mutex_lock(m)
#define lockdep_set_class(l, k) do { } while (0)

/* Semaphores. */
struct semaphore {
    unsigned long opaque[6];
    long count;
};
void sema_init(struct semaphore* s, int count);
void down(struct semaphore* s);
int down_interruptible(struct semaphore* s);
int down_trylock(struct semaphore* s);
int down_timeout(struct semaphore* s, long jiffies_timeout);
void up(struct semaphore* s);

/* Reader/writer semaphore: an exclusive mutex (correct, less concurrent). */
struct rw_semaphore {
    struct mutex m;
};
#define init_rwsem(s) mutex_init(&(s)->m)
#define down_read(s) mutex_lock(&(s)->m)
#define up_read(s) mutex_unlock(&(s)->m)
#define down_write(s) mutex_lock(&(s)->m)
#define up_write(s) mutex_unlock(&(s)->m)
#define DECLARE_RWSEM(name) struct rw_semaphore name = {__MUTEX_INITIALIZER(name)}

LKPI_EXTERN_C_END

#endif
