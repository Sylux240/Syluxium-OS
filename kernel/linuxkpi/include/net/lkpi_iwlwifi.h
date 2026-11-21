/* LinuxKPI: what Intel's iwlwifi/iwlmvm (Linux 6.12) expect the kernel
 * headers to provide beyond the common LinuxKPI set. Included first into
 * every iwlwifi source (Makefile). */
#pragma once

#include <linux/lkpi_all.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/in6.h>
#include <linux/device/bus.h>
#include <linux/devcoredump.h>
#include <linux/cleanup.h>
#include <net/lkpi_netcompat.h>


#include <linux/netdevice.h>
#include <linux/pci.h>

/* Usable in __aligned(): a constant expression for constant arguments. */
#define lkpi_iwl_roundup_pow_of_two(n) \
    ((u64)(n) <= 1 ? 1ULL : 1ULL << (64 - __builtin_clzll((unsigned long long)(n) - 1)))
#undef __roundup_pow_of_two
#define roundup_pow_of_two(n) lkpi_iwl_roundup_pow_of_two(n)

#define spin_trylock_bh(l) spin_trylock(l)
#define atomic_fetch_add_unless(v, a, u)                                                       \
    ({                                                                                     \
        int __c = atomic_read(v);                                                          \
        while (__c != (u) && !atomic_try_cmpxchg(v, &__c, __c + (a)))                      \
            ;                                                                              \
        __c;                                                                               \
    })


struct pci_bus;

#define kvmemdup(src, len, gfp) kmemdup(src, len, gfp)
#define dmam_pool_create dma_pool_create
#define pcim_iomap_regions_request_all(pdev, mask, name) pcim_iomap_regions(pdev, mask, name)
#define device_reprobe(dev) 0
#define HWEIGHT32(w) __builtin_popcount((u32)(w))
#define csum_unfold(x) ((__wsum)(x))

/* A net_device that exists only to own NAPI contexts. */
static inline void lkpi_dummy_netdev_setup(struct net_device *dev) { }
#define alloc_netdev_dummy(priv) alloc_netdev(priv, "dummy%d", 0, lkpi_dummy_netdev_setup)

static inline u64 mul_u64_u64_div_u64(u64 a, u64 b, u64 c)
{
    return (u64)(((unsigned __int128)a * b) / c);
}

#include <linux/tcp.h>

/* The op mode (iwlmvm) is built in and registers itself at init. */
#define request_module_nowait(fmt, ...) 0

#define devm_kasprintf(dev, gfp, fmt, ...) kasprintf(gfp, fmt, ##__VA_ARGS__)
#define __le32_to_cpup(p) le32_to_cpu(*(const __le32 *)(p))
#define __le16_to_cpup(p) le16_to_cpu(*(const __le16 *)(p))

static inline int atomic_dec_if_positive(atomic_t *v)
{
    int c = atomic_read(v);
    do {
        if (c <= 0)
            break;
    } while (!atomic_try_cmpxchg(v, &c, c - 1));
    return c - 1;
}

/* MSI-X (one interrupt per RX queue) isn't offered: iwlwifi falls back to
 * MSI and a single RX queue. CPU masks only matter for MSI-X affinity. */
typedef struct cpumask { unsigned long bits[1]; } cpumask_t;
#define cpu_online_mask ((const struct cpumask *)0)
static inline void cpumask_set_cpu(unsigned int cpu, struct cpumask *m) { }
static inline unsigned int cpumask_next(int n, const struct cpumask *m) { return n + 1; }
static inline int irq_set_affinity_hint(unsigned int irq, const struct cpumask *m) { return 0; }

/* PCI hot-remove/rescan (iwlwifi's last resort for a dead device) and PME. */
static inline void pci_lock_rescan_remove(void) { }
static inline void pci_unlock_rescan_remove(void) { }
static inline void pci_stop_and_remove_bus_device(struct pci_dev *dev) { }
static inline unsigned int pci_rescan_bus(struct pci_bus *bus) { return 0; }
static inline bool pci_pme_capable(struct pci_dev *dev, int state) { return false; }
static inline int pci_assign_resource(struct pci_dev *dev, int res) { return 0; }


static inline size_t sg_pcopy_from_buffer(struct scatterlist *sgl, unsigned int nents, const void *buf,
                                          size_t buflen, off_t skip)
{
    struct scatterlist *sg;
    size_t done = 0;
    int i;
    for_each_sg(sgl, sg, nents, i) {
        size_t len = sg->length, off = 0, n;
        if (skip >= (off_t)len) {
            skip -= len;
            continue;
        }
        off = skip;
        skip = 0;
        n = len - off < buflen - done ? len - off : buflen - done;
        if (sg_page(sg))
            memcpy((char *)page_address(sg_page(sg)) + sg->offset + off, (const char *)buf + done, n);
        done += n;
        if (done == buflen)
            break;
    }
    return done;
}

/* Firmware error dumps: no devcoredump to hand them to; free the pages. */
static inline void lkpi_iwl_free_sgtable(struct scatterlist *table)
{
    struct scatterlist *sg;
    int n = sg_nents(table), i;
    for_each_sg(table, sg, n, i)
        if (sg_page(sg))
            __free_page(sg_page(sg));
    kfree(table);
}
#define dev_coredumpsg(dev, table, datalen, gfp) lkpi_iwl_free_sgtable(table)
