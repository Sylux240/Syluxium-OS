/* LinuxKPI: hardware random number generators. Syluxium's kernel entropy
 * doesn't take driver RNGs yet: registration is accepted and unused. */
#pragma once
#include <linux/lkpi_all.h>

struct hwrng {
    const char *name;
    int (*init)(struct hwrng *rng);
    void (*cleanup)(struct hwrng *rng);
    int (*data_present)(struct hwrng *rng, int wait);
    int (*data_read)(struct hwrng *rng, u32 *data);
    int (*read)(struct hwrng *rng, void *data, size_t max, bool wait);
    unsigned long priv;
    unsigned short quality;
};

static inline int hwrng_register(struct hwrng *rng) { return 0; }
static inline void hwrng_unregister(struct hwrng *rng) { }
static inline int devm_hwrng_register(struct device *dev, struct hwrng *rng) { return 0; }
