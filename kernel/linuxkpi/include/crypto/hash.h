/* LinuxKPI: synchronous hashes / MACs ("cmac(aes)"). */
#ifndef _LKPI_CRYPTO_HASH_H
#define _LKPI_CRYPTO_HASH_H

#include <linux/crypto.h>

LKPI_EXTERN_C_BEGIN

struct crypto_shash {
    struct lkpi_crypto c;
};

struct shash_desc {
    struct crypto_shash* tfm;
    /* running CMAC state */
    u8 lkpi_x[16];
    u8 lkpi_buf[16];
    unsigned int lkpi_buflen;
    void* __ctx[] CRYPTO_MINALIGN_ATTR;
};

#define SHASH_DESC_ON_STACK(shash, ctx) \
    char __##shash##_desc[sizeof(struct shash_desc)] __aligned(8); \
    struct shash_desc* shash = (struct shash_desc*)__##shash##_desc; \
    shash->tfm = (ctx)

struct crypto_shash* crypto_alloc_shash(const char* alg_name, u32 type, u32 mask);
void crypto_free_shash(struct crypto_shash* tfm);
int crypto_shash_setkey(struct crypto_shash* tfm, const u8* key, unsigned int keylen);
int crypto_shash_init(struct shash_desc* desc);
int crypto_shash_update(struct shash_desc* desc, const u8* data, unsigned int len);
int crypto_shash_final(struct shash_desc* desc, u8* out);
int crypto_shash_finup(struct shash_desc* desc, const u8* data, unsigned int len, u8* out);
int crypto_shash_digest(struct shash_desc* desc, const u8* data, unsigned int len, u8* out);
static inline unsigned int crypto_shash_digestsize(struct crypto_shash* tfm) { return 16; }
static inline unsigned int crypto_shash_descsize(struct crypto_shash* tfm) { return 0; }
static inline void shash_desc_zero(struct shash_desc* desc) { memset(desc, 0, sizeof(*desc)); }

LKPI_EXTERN_C_END

#endif
