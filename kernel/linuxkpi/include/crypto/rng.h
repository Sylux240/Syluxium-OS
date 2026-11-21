/* LinuxKPI: random number generators: the kernel's random bytes. */
#ifndef _LKPI_CRYPTO_RNG_H
#define _LKPI_CRYPTO_RNG_H
#include <linux/kernel.h>
#include <linux/random.h>

struct crypto_rng {
    int unused;
};
static struct crypto_rng lkpi_default_rng __attribute__((unused));
static inline int crypto_get_default_rng(void) { return 0; }
static inline void crypto_put_default_rng(void) {}
#define crypto_default_rng (&lkpi_default_rng)
static inline int crypto_rng_get_bytes(struct crypto_rng *tfm, u8 *rdata, unsigned int dlen)
{
    get_random_bytes(rdata, dlen);
    return 0;
}

#endif
