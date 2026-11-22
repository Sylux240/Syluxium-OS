/* LinuxKPI: CPU masks and interrupt affinity.
 *
 * Linux drivers see one CPU (linux/percpu.h: nr_cpu_ids 1) and get one
 * interrupt vector per device, so a mask is a small bitmap, every "online"
 * mask holds CPU 0, and affinity hints are accepted and ignored: the vector
 * goes where Syluxium's interrupt code sends it. Drivers that spread
 * MSI-X vectors over CPUs (storage, multi-queue NICs) then run with one
 * queue, which they all support. */
#pragma once
#include <linux/lkpi_all.h>

#define LKPI_NR_CPUS 64
typedef struct cpumask { unsigned long bits[BITS_TO_LONGS(LKPI_NR_CPUS)]; } cpumask_t;
typedef struct cpumask cpumask_var_t[1]; /* as with CONFIG_CPUMASK_OFFSTACK=n */
#define cpumask_bits(m) ((m)->bits)
#define nr_cpumask_bits nr_cpu_ids

static const struct cpumask lkpi_cpu_mask_cpu0 = { { 1ul } };
#define cpu_online_mask (&lkpi_cpu_mask_cpu0)
#define cpu_possible_mask (&lkpi_cpu_mask_cpu0)
#define cpu_present_mask (&lkpi_cpu_mask_cpu0)
#define cpu_active_mask (&lkpi_cpu_mask_cpu0)
#define cpu_all_mask (&lkpi_cpu_mask_cpu0)
static inline const struct cpumask *cpumask_of(unsigned int cpu) { return &lkpi_cpu_mask_cpu0; }
static inline const struct cpumask *cpumask_of_node(int node) { return &lkpi_cpu_mask_cpu0; }
#define cpu_online(cpu) ((cpu) == 0)

static inline bool alloc_cpumask_var(cpumask_var_t *mask, gfp_t flags) { return true; }
static inline bool zalloc_cpumask_var(cpumask_var_t *mask, gfp_t flags)
{
	memset(*mask, 0, sizeof(**mask));
	return true;
}
#define zalloc_cpumask_var_node(m, f, n) zalloc_cpumask_var(m, f)
#define alloc_cpumask_var_node(m, f, n) alloc_cpumask_var(m, f)
static inline void free_cpumask_var(cpumask_var_t mask) { }

static inline void cpumask_set_cpu(unsigned int cpu, struct cpumask *m) { if (cpu < LKPI_NR_CPUS) __set_bit(cpu, m->bits); }
static inline void cpumask_clear_cpu(int cpu, struct cpumask *m) { if (cpu < LKPI_NR_CPUS) __clear_bit(cpu, m->bits); }
static inline bool cpumask_test_cpu(int cpu, const struct cpumask *m) { return cpu < LKPI_NR_CPUS && test_bit(cpu, m->bits); }
static inline void cpumask_clear(struct cpumask *m) { memset(m, 0, sizeof(*m)); }
static inline void cpumask_setall(struct cpumask *m) { cpumask_clear(m); __set_bit(0, m->bits); }
static inline void cpumask_copy(struct cpumask *d, const struct cpumask *s) { *d = *s; }
static inline bool cpumask_empty(const struct cpumask *m) { return find_first_bit(m->bits, LKPI_NR_CPUS) >= LKPI_NR_CPUS; }
static inline unsigned int cpumask_first(const struct cpumask *m) { return find_first_bit(m->bits, LKPI_NR_CPUS); }
static inline unsigned int cpumask_next(int n, const struct cpumask *m) { return find_next_bit(m->bits, LKPI_NR_CPUS, n + 1); }
static inline unsigned int cpumask_weight(const struct cpumask *m)
{
	unsigned int i, w = 0;
	for (i = 0; i < BITS_TO_LONGS(LKPI_NR_CPUS); i++)
		w += __builtin_popcountl(m->bits[i]);
	return w;
}
static inline bool cpumask_and(struct cpumask *d, const struct cpumask *a, const struct cpumask *b)
{
	unsigned int i;
	bool any = false;
	for (i = 0; i < BITS_TO_LONGS(LKPI_NR_CPUS); i++)
		any |= (d->bits[i] = a->bits[i] & b->bits[i]) != 0;
	return any;
}
static inline void cpumask_or(struct cpumask *d, const struct cpumask *a, const struct cpumask *b)
{
	unsigned int i;
	for (i = 0; i < BITS_TO_LONGS(LKPI_NR_CPUS); i++)
		d->bits[i] = a->bits[i] | b->bits[i];
}
static inline unsigned int cpumask_local_spread(unsigned int i, int node) { return 0; }
static inline unsigned int cpumask_any(const struct cpumask *m) { return cpumask_first(m); }
#define for_each_cpu(cpu, mask) \
	for ((cpu) = cpumask_first(mask); (cpu) < nr_cpu_ids; (cpu) = cpumask_next((cpu), (mask)))
#define for_each_cpu_and(cpu, a, b) for_each_cpu(cpu, a) if (cpumask_test_cpu((cpu), (b)))

/* ---- interrupt affinity (include/linux/interrupt.h) ---- */
struct irq_affinity {
	unsigned int pre_vectors;
	unsigned int post_vectors;
	unsigned int nr_sets;
	unsigned int set_size[4];
	void (*calc_sets)(struct irq_affinity *, unsigned int nvecs);
	void *priv;
};
struct irq_affinity_desc {
	struct cpumask mask;
	unsigned int is_managed : 1;
};
static inline int irq_set_affinity_hint(unsigned int irq, const struct cpumask *m) { return 0; }
static inline int irq_update_affinity_hint(unsigned int irq, const struct cpumask *m) { return 0; }
static inline int irq_set_affinity_and_hint(unsigned int irq, const struct cpumask *m) { return 0; }
static inline int irq_set_affinity(unsigned int irq, const struct cpumask *m) { return 0; }
static inline int irq_force_affinity(unsigned int irq, const struct cpumask *m) { return 0; }

/* ---- PCI vectors with affinity (one vector per device here: see above) ---- */
struct pci_dev;
int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs, unsigned int max_vecs, unsigned int flags);
#ifndef PCI_IRQ_AFFINITY
#define PCI_IRQ_AFFINITY 0x8
#endif
static inline int pci_alloc_irq_vectors_affinity(struct pci_dev *dev, unsigned int min_vecs, unsigned int max_vecs,
						 unsigned int flags, struct irq_affinity *affd)
{
	/* Vectors reserved before/after the spread ones (pre/post_vectors) still
	 * count: a driver asking for at least two gets -ENOSPC and falls back. */
	return pci_alloc_irq_vectors(dev, min_vecs, max_vecs, flags & ~PCI_IRQ_AFFINITY);
}
static inline const struct cpumask *pci_irq_get_affinity(struct pci_dev *dev, int vec) { return cpu_online_mask; }
