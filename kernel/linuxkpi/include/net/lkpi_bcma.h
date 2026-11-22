/* LinuxKPI: what Broadcom's bcma bus and b43 (Linux 6.12) expect from the
 * kernel headers beyond the common LinuxKPI set. Included first (Makefile). */
#pragma once

#include <net/lkpi_netcompat.h>
#include <linux/device/bus.h>
#include <linux/devres.h>

/* Repeated MMIO reads/writes to one register (block I/O to a FIFO). */
#define LKPI_IO_REP(name, type, op)                                                                \
    static inline void ioread##name##_rep(const volatile void __iomem *addr, void *buf, unsigned long n) \
    {                                                                                          \
        type *b = buf;                                                                         \
        while (n--)                                                                            \
            *b++ = ioread##name(addr);                                                         \
    }                                                                                          \
    static inline void iowrite##name##_rep(volatile void __iomem *addr, const void *buf, unsigned long n) \
    {                                                                                          \
        const type *b = buf;                                                                   \
        while (n--)                                                                            \
            iowrite##name(*b++, addr);                                                         \
    }
#ifndef ioread8_rep
LKPI_IO_REP(8, u8, 0)
LKPI_IO_REP(16, u16, 0)
LKPI_IO_REP(32, u32, 0)
#endif

/* Device IDs of the SSB and BCMA buses (linux/mod_devicetable.h). */
#ifndef LKPI_SSB_DEVICE_ID
#define LKPI_SSB_DEVICE_ID
struct ssb_device_id {
    __u16 vendor;
    __u16 coreid;
    __u8 revision;
    __u8 __pad;
} __attribute__((packed, aligned(2)));
#define SSB_DEVICE(_vendor, _coreid, _revision) { .vendor = _vendor, .coreid = _coreid, .revision = _revision, }
#define SSB_ANY_VENDOR 0xFFFF
#define SSB_ANY_ID 0xFFFF
#define SSB_ANY_REV 0xFF
#endif

struct bcma_device_id {
    __u16 manuf;
    __u16 id;
    __u8 rev;
    __u8 class;
} __attribute__((packed, aligned(2)));
#define BCMA_CORE(_manuf, _id, _rev, _class) { .manuf = _manuf, .id = _id, .rev = _rev, .class = _class, }
#define BCMA_ANY_MANUF 0xFFFF
#define BCMA_ANY_ID 0xFFFF
#define BCMA_ANY_REV 0xFF
#define BCMA_ANY_CLASS 0xFF
