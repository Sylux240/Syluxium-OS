#ifndef _LKPI_LINUX_IO_H
#define _LKPI_LINUX_IO_H

#include <linux/kernel.h>

LKPI_EXTERN_C_BEGIN

/* MMIO accessors (uncached mappings; x86 keeps device accesses ordered). */
static inline u8 readb(const volatile void __iomem* a) { return *(const volatile u8*)a; }
static inline u16 readw(const volatile void __iomem* a) { return *(const volatile u16*)a; }
static inline u32 readl(const volatile void __iomem* a) { return *(const volatile u32*)a; }
static inline u64 readq(const volatile void __iomem* a) { return *(const volatile u64*)a; }
static inline void writeb(u8 v, volatile void __iomem* a) { *(volatile u8*)a = v; }
static inline void writew(u16 v, volatile void __iomem* a) { *(volatile u16*)a = v; }
static inline void writel(u32 v, volatile void __iomem* a) { *(volatile u32*)a = v; }
static inline void writeq(u64 v, volatile void __iomem* a) { *(volatile u64*)a = v; }
#define readq readq
#define writeq writeq
#define readq_relaxed readq
#define writeq_relaxed writeq
#define readb_relaxed readb
#define readw_relaxed readw
#define readl_relaxed readl
#define writeb_relaxed writeb
#define writew_relaxed writew
#define writel_relaxed writel
#define ioread8 readb
#define ioread16 readw
#define ioread32 readl
#define iowrite8 writeb
#define iowrite16 writew
#define iowrite32 writel
#define __raw_readl readl
#define __raw_writel writel
/* Device memory copies. Small ones use 4-byte accesses like Linux on x86
 * (register blocks); bulk copies (4 KiB and up: VRAM and apertures) use
 * 8-byte ones. Uncached PCIe reads cost about a microsecond each: amdgpu
 * copies 32 MiB of VRAM around memory training, which byte accesses
 * stretched to 40 s. */
#define LKPI_IO_COPY(T, d, s, n)                                         \
    for (; (n) >= sizeof(T); (n) -= sizeof(T), (s) += sizeof(T), (d) += sizeof(T))
static inline void memcpy_fromio(void* dst, const volatile void __iomem* src, size_t n) {
    u8* d = (u8*)dst;
    const volatile u8* s = (const volatile u8*)src;
    while (n && ((unsigned long)s & 3)) { *d++ = *s++; n--; }
    if (n >= 4096) {
        if (((unsigned long)s & 7) && n >= 4) { u32 v = *(const volatile u32*)s; __builtin_memcpy(d, &v, 4); s += 4; d += 4; n -= 4; }
        LKPI_IO_COPY(u64, d, s, n) { u64 v = *(const volatile u64*)s; __builtin_memcpy(d, &v, 8); }
    }
    LKPI_IO_COPY(u32, d, s, n) { u32 v = *(const volatile u32*)s; __builtin_memcpy(d, &v, 4); }
    while (n--) *d++ = *s++;
}
static inline void memcpy_toio(volatile void __iomem* dst, const void* src, size_t n) {
    volatile u8* d = (volatile u8*)dst;
    const u8* s = (const u8*)src;
    while (n && ((unsigned long)d & 3)) { *d++ = *s++; n--; }
    if (n >= 4096) {
        if (((unsigned long)d & 7) && n >= 4) { u32 v; __builtin_memcpy(&v, s, 4); *(volatile u32*)d = v; s += 4; d += 4; n -= 4; }
        LKPI_IO_COPY(u64, d, s, n) { u64 v; __builtin_memcpy(&v, s, 8); *(volatile u64*)d = v; }
    }
    LKPI_IO_COPY(u32, d, s, n) { u32 v; __builtin_memcpy(&v, s, 4); *(volatile u32*)d = v; }
    while (n--) *d++ = *s++;
}
static inline void memset_io(volatile void __iomem* dst, int v, size_t n) {
    volatile u8* d = (volatile u8*)dst;
    u32 w = 0x01010101u * (u8)v;
    while (n && ((unsigned long)d & 3)) { *d++ = (u8)v; n--; }
    if (n >= 4096) {
        if (((unsigned long)d & 7) && n >= 4) { *(volatile u32*)d = w; d += 4; n -= 4; }
        for (; n >= 8; n -= 8, d += 8) *(volatile u64*)d = ((u64)w << 32) | w;
    }
    for (; n >= 4; n -= 4, d += 4) *(volatile u32*)d = w;
    while (n--) *d++ = (u8)v;
}

/* Port I/O. */
static inline u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u16 inw(u16 port) { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u32 inl(u16 port) { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outb(u8 v, u16 port) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline void outw(u16 v, u16 port) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
static inline void outl(u32 v, u16 port) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }

void __iomem* ioremap(phys_addr_t phys, size_t size);
#define ioremap_nocache ioremap
#define ioremap_uc ioremap
void __iomem* ioremap_wc(phys_addr_t phys, size_t size);
void __iomem* ioremap_cache(phys_addr_t phys, size_t size);
#define ioremap_np ioremap
#define memremap(p, s, f) ((void *)ioremap_cache(p, s))
#define memunmap(v) iounmap(v)
#define MEMREMAP_WB 1
#define MEMREMAP_WC 4
#define arch_phys_wc_add(base, size) 0
#define arch_phys_wc_del(h) do { } while (0)
#define arch_io_reserve_memtype_wc(base, size) 0
#define arch_io_free_memtype_wc(base, size) do { } while (0)
#define devm_arch_phys_wc_add(dev, base, size) 0
#define devm_arch_io_reserve_memtype_wc(dev, base, size) 0
void iounmap(volatile void __iomem* addr);
#define devm_ioremap(dev, p, s) ioremap(p, s)

LKPI_EXTERN_C_END

#endif
