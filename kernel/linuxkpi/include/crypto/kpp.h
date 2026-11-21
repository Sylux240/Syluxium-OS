/* LinuxKPI: key agreement (kpp). Only "ecdh-nist-p256" (Bluetooth Secure
 * Connections), computed synchronously with Linux's ecc.c (glue/kpp.c). */
#ifndef _LKPI_CRYPTO_KPP_H
#define _LKPI_CRYPTO_KPP_H
#include <linux/kernel.h>
#include <linux/scatterlist.h>
#include <crypto/hash.h>

LKPI_EXTERN_C_BEGIN

struct crypto_kpp {
    unsigned int curve_id;
    unsigned int ndigits;
    u64 private_key[8];
};

struct kpp_request {
    struct crypto_kpp *tfm;
    struct scatterlist *src;
    struct scatterlist *dst;
    unsigned int src_len;
    unsigned int dst_len;
};

struct crypto_kpp *crypto_alloc_kpp(const char *alg_name, u32 type, u32 mask);
void crypto_free_kpp(struct crypto_kpp *tfm);
int crypto_kpp_set_secret(struct crypto_kpp *tfm, const void *buffer, unsigned int len);
int crypto_kpp_generate_public_key(struct kpp_request *req);
int crypto_kpp_compute_shared_secret(struct kpp_request *req);
static inline unsigned int crypto_kpp_maxsize(struct crypto_kpp *tfm) { return tfm->ndigits * 8 * 2; }

static inline struct kpp_request *kpp_request_alloc(struct crypto_kpp *tfm, gfp_t gfp)
{
    struct kpp_request *req = kzalloc(sizeof(*req), gfp);
    if (req)
        req->tfm = tfm;
    return req;
}
static inline void kpp_request_free(struct kpp_request *req) { kfree_sensitive(req); }
static inline void kpp_request_set_input(struct kpp_request *req, struct scatterlist *input, unsigned int input_len)
{
    req->src = input;
    req->src_len = input_len;
}
static inline void kpp_request_set_output(struct kpp_request *req, struct scatterlist *output, unsigned int output_len)
{
    req->dst = output;
    req->dst_len = output_len;
}
/* Requests complete at once here. */
#define kpp_request_set_callback(req, flags, cmpl, data) do { } while (0)

LKPI_EXTERN_C_END
#endif
