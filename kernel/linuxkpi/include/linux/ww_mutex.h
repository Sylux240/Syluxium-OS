/* LinuxKPI: wound/wait mutexes, simplified - plain mutexes without Linux's
 * deadlock back-off (-EDEADLK is never returned). Enough for DRM drivers
 * that take their modeset/reservation locks in a consistent order. */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/mutex.h>

struct ww_class { const char *name; };
struct ww_acquire_ctx { struct ww_class *ww_class; unsigned int acquired; };
struct ww_mutex { struct mutex base; struct ww_acquire_ctx *ctx; struct ww_class *ww_class; };

#define DEFINE_WW_CLASS(classname) struct ww_class classname = { .name = #classname }
#define DEFINE_WD_CLASS(classname) DEFINE_WW_CLASS(classname)

static inline void ww_mutex_init(struct ww_mutex *lock, struct ww_class *ww_class)
{
	mutex_init(&lock->base);
	lock->ctx = NULL;
	lock->ww_class = ww_class;
}
static inline void ww_acquire_init(struct ww_acquire_ctx *ctx, struct ww_class *ww_class)
{
	ctx->ww_class = ww_class;
	ctx->acquired = 0;
}
static inline void ww_acquire_done(struct ww_acquire_ctx *ctx) { (void)ctx; }
static inline void ww_acquire_fini(struct ww_acquire_ctx *ctx) { (void)ctx; }
static inline int ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	if (ctx && lock->ctx == ctx)
		return -EALREADY;
	mutex_lock(&lock->base);
	lock->ctx = ctx;
	if (ctx)
		ctx->acquired++;
	return 0;
}
static inline int ww_mutex_lock_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	int r;
	if (ctx && lock->ctx == ctx)
		return -EALREADY;
	r = mutex_lock_interruptible(&lock->base);
	if (r)
		return r;
	lock->ctx = ctx;
	if (ctx)
		ctx->acquired++;
	return 0;
}
static inline void ww_mutex_lock_slow(struct ww_mutex *lock, struct ww_acquire_ctx *ctx) { (void)ww_mutex_lock(lock, ctx); }
static inline int ww_mutex_lock_slow_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	return ww_mutex_lock_interruptible(lock, ctx);
}
static inline int ww_mutex_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	if (!mutex_trylock(&lock->base))
		return 0;
	lock->ctx = ctx;
	if (ctx)
		ctx->acquired++;
	return 1;
}
static inline void ww_mutex_unlock(struct ww_mutex *lock)
{
	if (lock->ctx)
		lock->ctx->acquired--;
	lock->ctx = NULL;
	mutex_unlock(&lock->base);
}
static inline bool ww_mutex_is_locked(struct ww_mutex *lock) { return mutex_is_locked(&lock->base); }
static inline void ww_mutex_destroy(struct ww_mutex *lock) { (void)lock; }
#define ww_mutex_base_lock(l) mutex_lock(&(l)->base)
