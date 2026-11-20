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
static inline void memcpy_fromio(void* dst, const volatile void __iomem* src, size_t n) {
    for (size_t i = 0; i < n; i++) ((u8*)dst)[i] = ((const volatile u8*)src)[i];
}
static inline void memcpy_toio(volatile void __iomem* dst, const void* src, size_t n) {
    for (size_t i = 0; i < n; i++) ((volatile u8*)dst)[i] = ((const u8*)src)[i];
}
static inline void memset_io(volatile void __iomem* dst, int v, size_t n) {
    for (size_t i = 0; i < n; i++) ((volatile u8*)dst)[i] = (u8)v;
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
