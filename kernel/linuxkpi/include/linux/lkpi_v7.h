/* Linux 7.x kernel APIs on top of the LinuxKPI's 6.12-era headers.
 *
 * Drivers imported from the pinned upstream tree (Linux 7.4-rc3, see
 * docs/drivers.md) are force-included with this header (Makefile:
 * LKPI_DRV_CFLAGS), so the shared headers the 6.12 imports use stay as they
 * are. Only generic core APIs live here; subsystem extras go into their own
 * header (net/lkpi_netdrv.h for Ethernet drivers, ...). */
#pragma once
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/timer.h>
#include <linux/pci.h>

/* ---- typed allocation helpers (include/linux/slab.h, 7.0) ---- */
#ifndef default_gfp
#define __lkpi_default_gfp(a, b, ...) b
#define default_gfp(...) __lkpi_default_gfp(, ##__VA_ARGS__, GFP_KERNEL)
#endif
#define __lkpi_alloc_objs(KMALLOC, GFP, TYPE, COUNT) \
	((TYPE *)KMALLOC(lkpi_size_mul(sizeof(TYPE), (COUNT)), GFP))
#define __lkpi_alloc_flex(KMALLOC, GFP, TYPE, FAM, COUNT) \
	((TYPE *)KMALLOC(sizeof(TYPE) + lkpi_size_mul(sizeof(((TYPE *)0)->FAM[0]), (COUNT)), GFP))
static inline size_t lkpi_size_mul(size_t a, size_t b)
{
	size_t r;
	return __builtin_mul_overflow(a, b, &r) ? (size_t)-1 : r;
}
#define kmalloc_obj(T, ...) __lkpi_alloc_objs(kmalloc, default_gfp(__VA_ARGS__), typeof(T), 1)
#define kmalloc_objs(T, N, ...) __lkpi_alloc_objs(kmalloc, default_gfp(__VA_ARGS__), typeof(T), N)
#define kmalloc_flex(T, FAM, N, ...) __lkpi_alloc_flex(kmalloc, default_gfp(__VA_ARGS__), typeof(T), FAM, N)
#define kzalloc_obj(T, ...) __lkpi_alloc_objs(kzalloc, default_gfp(__VA_ARGS__), typeof(T), 1)
#define kzalloc_objs(T, N, ...) __lkpi_alloc_objs(kzalloc, default_gfp(__VA_ARGS__), typeof(T), N)
#define kzalloc_flex(T, FAM, N, ...) __lkpi_alloc_flex(kzalloc, default_gfp(__VA_ARGS__), typeof(T), FAM, N)
#define kvmalloc_obj(T, ...) __lkpi_alloc_objs(kvmalloc, default_gfp(__VA_ARGS__), typeof(T), 1)
#define kvmalloc_objs(T, N, ...) __lkpi_alloc_objs(kvmalloc, default_gfp(__VA_ARGS__), typeof(T), N)
#define kvmalloc_flex(T, FAM, N, ...) __lkpi_alloc_flex(kvmalloc, default_gfp(__VA_ARGS__), typeof(T), FAM, N)
#define kvzalloc_obj(T, ...) __lkpi_alloc_objs(kvzalloc, default_gfp(__VA_ARGS__), typeof(T), 1)
#define kvzalloc_objs(T, N, ...) __lkpi_alloc_objs(kvzalloc, default_gfp(__VA_ARGS__), typeof(T), N)
#define kvzalloc_flex(T, FAM, N, ...) __lkpi_alloc_flex(kvzalloc, default_gfp(__VA_ARGS__), typeof(T), FAM, N)

#ifndef vmalloc_array
#define vmalloc_array(n, size) vmalloc(lkpi_size_mul((n), (size)))
#endif
#ifndef vcalloc
#define vcalloc(n, size) vzalloc(lkpi_size_mul((n), (size)))
#endif

/* ---- timers (7.0: from_timer became timer_container_of) ---- */
#ifndef timer_container_of
#define timer_container_of(var, t, field) container_of(t, typeof(*var), field)
#endif

/* ---- bitops ---- */
#ifndef for_each_set_bit_from
#define for_each_set_bit_from(bit, addr, size) \
	for ((bit) = find_next_bit((addr), (size), (bit)); (bit) < (size); (bit) = find_next_bit((addr), (size), (bit) + 1))
#endif

/* ---- byte swapping in place ---- */
#ifndef swab32s
static inline void swab16s(u16 *p) { *p = __builtin_bswap16(*p); }
static inline void swab32s(u32 *p) { *p = __builtin_bswap32(*p); }
static inline void swab64s(u64 *p) { *p = __builtin_bswap64(*p); }
#define swab32s swab32s
#endif

/* ---- PCI id tables ---- */
#ifndef PCI_VDEVICE_SUB
#define PCI_VDEVICE_SUB(vend, dev, subvend, subdev) \
	.vendor = PCI_VENDOR_ID_##vend, .device = (dev), .subvendor = (subvend), .subdevice = (subdev), 0, 0
#endif

/* ---- scope-based lock guards (guard(mutex)(&m), scoped_guard(spinlock_irqsave, &l) {...}),
 * as include/linux/{mutex,spinlock,rcupdate}.h define them ---- */
#include <linux/cleanup.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#ifndef LKPI_LOCK_GUARDS
#define LKPI_LOCK_GUARDS 1
DEFINE_GUARD(mutex, struct mutex *, mutex_lock(_T), mutex_unlock(_T))
DEFINE_LOCK_GUARD_1(spinlock, spinlock_t, spin_lock(_T->lock), spin_unlock(_T->lock))
DEFINE_LOCK_GUARD_1(spinlock_irq, spinlock_t, spin_lock_irq(_T->lock), spin_unlock_irq(_T->lock))
DEFINE_LOCK_GUARD_1(spinlock_bh, spinlock_t, spin_lock_bh(_T->lock), spin_unlock_bh(_T->lock))
DEFINE_LOCK_GUARD_1(spinlock_irqsave, spinlock_t, spin_lock_irqsave(_T->lock, _T->flags),
		    spin_unlock_irqrestore(_T->lock, _T->flags), unsigned long flags)
DEFINE_LOCK_GUARD_0(rcu, rcu_read_lock(), rcu_read_unlock())
#endif

/* ---- PCI odds and ends ---- */
#ifndef MDIO_NAME_SIZE /* net/lkpi_phy.h (included first where used) has its own */
static inline int pci_select_bars(struct pci_dev *dev, unsigned long flags)
{
	int i, bars = 0;
	for (i = 0; i < 6; i++)
		if (pci_resource_flags(dev, i) & flags)
			bars |= 1 << i;
	return bars;
}
#endif
/* Config space is never blocked here (no user space config access to race with). */
static inline void pci_cfg_access_lock(struct pci_dev *dev) { }
static inline void pci_cfg_access_unlock(struct pci_dev *dev) { }
static inline bool pci_cfg_access_trylock(struct pci_dev *dev) { return true; }

/* ---- struct helpers (linux/stddef.h) ---- */
#ifndef TRAILING_OVERLAP
#define __TRAILING_OVERLAP(TYPE, NAME, FAM, ATTRS, MEMBERS) \
	union { \
		TYPE NAME; \
		struct { \
			unsigned char __offset_to_FAM[offsetof(TYPE, FAM)]; \
			MEMBERS \
		} ATTRS; \
	}
#define TRAILING_OVERLAP(TYPE, NAME, FAM, MEMBERS) __TRAILING_OVERLAP(TYPE, NAME, FAM, , MEMBERS)
#endif

/* ---- 32-bit compat ioctls: Linux drivers are never called from ia32 code here
 * (the ia32 layer translates the structures it knows, drivers get 64-bit ones) ---- */
#include <linux/uio.h>
#ifndef in_compat_syscall
#define in_compat_syscall() false
#endif
typedef u32 compat_uptr_t;
typedef u32 compat_size_t;
typedef s32 compat_int_t;
typedef u32 compat_uint_t;
typedef u32 compat_ulong_t;
struct compat_iovec { compat_uptr_t iov_base; compat_size_t iov_len; };

/* ---- SIGIO notification of a driver file (fasync): not delivered ---- */
struct fasync_struct;
struct file;
static inline int fasync_helper(int fd, struct file *filp, int on, struct fasync_struct **fapp) { return 0; }
static inline void kill_fasync(struct fasync_struct **fp, int sig, int band) { }
#ifndef SIGIO
#define SIGIO 29
#endif
#ifndef POLL_IN
#define POLL_IN 1
#endif

/* ---- kdump "reset_devices" boot option: never set ---- */
#define reset_devices 0

/* ---- driver attributes (sysfs files of a driver): not exposed ---- */
struct driver_attribute {
	struct attribute attr;
	ssize_t (*show)(struct device_driver *driver, char *buf);
	ssize_t (*store)(struct device_driver *driver, const char *buf, size_t count);
};
#define DRIVER_ATTR_RW(_name) struct driver_attribute driver_attr_##_name = __ATTR_RW(_name)
#define DRIVER_ATTR_RO(_name) struct driver_attribute driver_attr_##_name = __ATTR_RO(_name)
#define DRIVER_ATTR_WO(_name) struct driver_attribute driver_attr_##_name = __ATTR_WO(_name)
static inline int driver_create_file(const struct device_driver *drv, const struct driver_attribute *attr) { return 0; }
static inline void driver_remove_file(const struct device_driver *drv, const struct driver_attribute *attr) { }
