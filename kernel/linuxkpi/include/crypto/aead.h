/* LinuxKPI: authenticated encryption (synchronous; see linux/crypto.h). */
#ifndef _LKPI_CRYPTO_AEAD_H
#define _LKPI_CRYPTO_AEAD_H

#include <linux/crypto.h>

LKPI_EXTERN_C_BEGIN

struct crypto_aead {
    struct lkpi_crypto c;
};

struct aead_request {
    struct crypto_aead* tfm;
    struct scatterlist* src;
    struct scatterlist* dst;
    unsigned int cryptlen;
    unsigned int assoclen;
    u8* iv;
    u32 flags;
    void* __ctx[] CRYPTO_MINALIGN_ATTR;
};

struct crypto_aead* crypto_alloc_aead(const char* alg_name, u32 type, u32 mask);
void crypto_free_aead(struct crypto_aead* tfm);
int crypto_aead_setkey(struct crypto_aead* tfm, const u8* key, unsigned int keylen);
int crypto_aead_setauthsize(struct crypto_aead* tfm, unsigned int authsize);
int crypto_aead_encrypt(struct aead_request* req);
int crypto_aead_decrypt(struct aead_request* req);
static inline unsigned int crypto_aead_authsize(struct crypto_aead* tfm) { return tfm->c.authsize; }
static inline unsigned int crypto_aead_reqsize(struct crypto_aead* tfm) { return 0; }
static inline unsigned int crypto_aead_ivsize(struct crypto_aead* tfm) { return 16; }
static inline struct crypto_tfm* crypto_aead_tfm(struct crypto_aead* tfm) { return &tfm->c.base; }
static inline void aead_request_set_tfm(struct aead_request* req, struct crypto_aead* tfm) { req->tfm = tfm; }
static inline struct crypto_aead* crypto_aead_reqtfm(struct aead_request* req) { return req->tfm; }
static inline void aead_request_set_crypt(struct aead_request* req, struct scatterlist* src, struct scatterlist* dst,
                                          unsigned int cryptlen, u8* iv) {
    req->src = src;
    req->dst = dst;
    req->cryptlen = cryptlen;
    req->iv = iv;
}
static inline void aead_request_set_ad(struct aead_request* req, unsigned int assoclen) { req->assoclen = assoclen; }
static inline void aead_request_set_callback(struct aead_request* req, u32 flags, void* compl, void* data) { req->flags = flags; }
static inline struct aead_request* aead_request_alloc(struct crypto_aead* tfm, gfp_t gfp) {
    struct aead_request* req = (struct aead_request*)kzalloc(sizeof(*req), gfp);
    if (req) req->tfm = tfm;
    return req;
}
static inline void aead_request_free(struct aead_request* req) { kfree_sensitive(req); }

LKPI_EXTERN_C_END

#endif
