/* LinuxKPI: a small synchronous crypto API covering what the wireless stack
 * uses: AEAD "ccm(aes)" / "gcm(aes)" / "gmac(aes)", hash "cmac(aes)",
 * skcipher "ctr(aes)" (implemented in glue/crypto.c on lib/crypto/aes.c). */
#ifndef _LKPI_LINUX_CRYPTO_H
#define _LKPI_LINUX_CRYPTO_H

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/bitops.h>

LKPI_EXTERN_C_BEGIN

#define CRYPTO_ALG_ASYNC 0x00000080
#define CRYPTO_ALG_NEED_FALLBACK 0x00000100
#define CRYPTO_ALG_ALLOCATES_MEMORY 0x00010000
#define CRYPTO_TFM_REQ_MAY_SLEEP 0x00000200
#define CRYPTO_TFM_REQ_MAY_BACKLOG 0x00000400
#define CRYPTO_MINALIGN 8
#define CRYPTO_MINALIGN_ATTR __attribute__((__aligned__(CRYPTO_MINALIGN)))

struct crypto_tfm {
    u32 crt_flags;
    void* lkpi_ctx;
};

/* One transform object for every kind: the algorithm and its key state. */
struct lkpi_crypto {
    struct crypto_tfm base;
    int alg;               /* LKPI_ALG_* */
    unsigned int authsize;
    unsigned int keylen;
    u8 key[32];
    void* aes;             /* struct crypto_aes_ctx */
};

/* Every request here completes before the call returns: waiting is a no-op. */
struct crypto_wait {
    int err;
};
#define DECLARE_CRYPTO_WAIT(name) struct crypto_wait name = { 0 }
static inline int crypto_wait_req(int err, struct crypto_wait *wait) { return err; }
static inline void crypto_req_done(void *data, int err) {}

LKPI_EXTERN_C_END

#endif
