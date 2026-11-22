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
