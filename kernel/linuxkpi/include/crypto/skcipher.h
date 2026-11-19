/* LinuxKPI: synchronous symmetric ciphers ("ctr(aes)"). */
#ifndef _LKPI_CRYPTO_SKCIPHER_H
#define _LKPI_CRYPTO_SKCIPHER_H

#include <linux/crypto.h>

LKPI_EXTERN_C_BEGIN

struct crypto_skcipher {
    struct lkpi_crypto c;
};
struct crypto_sync_skcipher {
    struct lkpi_crypto c;
};

struct skcipher_request {
    struct crypto_skcipher* tfm;
    struct scatterlist* src;
    struct scatterlist* dst;
    unsigned int cryptlen;
    u8* iv;
    void* __ctx[] CRYPTO_MINALIGN_ATTR;
};

struct crypto_skcipher* crypto_alloc_skcipher(const char* alg_name, u32 type, u32 mask);
void crypto_free_skcipher(struct crypto_skcipher* tfm);
int crypto_skcipher_setkey(struct crypto_skcipher* tfm, const u8* key, unsigned int keylen);
int crypto_skcipher_encrypt(struct skcipher_request* req);
int crypto_skcipher_decrypt(struct skcipher_request* req);
static inline struct skcipher_request* skcipher_request_alloc(struct crypto_skcipher* tfm, gfp_t gfp) {
    struct skcipher_request* req = (struct skcipher_request*)kzalloc(sizeof(*req), gfp);
    if (req) req->tfm = tfm;
    return req;
}
static inline void skcipher_request_free(struct skcipher_request* req) { kfree_sensitive(req); }
static inline void skcipher_request_set_tfm(struct skcipher_request* req, struct crypto_skcipher* tfm) { req->tfm = tfm; }
static inline void skcipher_request_set_callback(struct skcipher_request* req, u32 flags, void* compl, void* data) {}
static inline void skcipher_request_set_crypt(struct skcipher_request* req, struct scatterlist* src,
                                              struct scatterlist* dst, unsigned int cryptlen, void* iv) {
    req->src = src;
    req->dst = dst;
    req->cryptlen = cryptlen;
    req->iv = (u8*)iv;
}

LKPI_EXTERN_C_END

#endif
