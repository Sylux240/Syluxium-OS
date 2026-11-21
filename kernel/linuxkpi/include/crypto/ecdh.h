/* LinuxKPI: ECDH key encoding (as Linux's crypto/ecdh_helper.c: a
 * kpp_secret header, the key size, then the key). */
#ifndef _LKPI_CRYPTO_ECDH_H
#define _LKPI_CRYPTO_ECDH_H
#include <linux/kernel.h>

#define ECC_CURVE_NIST_P192 0x0001
#define ECC_CURVE_NIST_P256 0x0002
#define ECC_CURVE_NIST_P384 0x0003
#define ECC_CURVE_NIST_P521 0x0004
#define CRYPTO_KPP_SECRET_TYPE_ECDH 2

struct ecdh {
    char *key;
    unsigned short key_size;
};

struct kpp_secret {
    unsigned short type;
    unsigned short len;
};

#define ECDH_KPP_SECRET_MIN_SIZE (sizeof(struct kpp_secret) + sizeof(unsigned short))

static inline unsigned int crypto_ecdh_key_len(const struct ecdh *params)
{
    return ECDH_KPP_SECRET_MIN_SIZE + params->key_size;
}

static inline int crypto_ecdh_encode_key(char *buf, unsigned int len, const struct ecdh *p)
{
    struct kpp_secret secret = { CRYPTO_KPP_SECRET_TYPE_ECDH, (unsigned short)len };
    if (!buf || len != crypto_ecdh_key_len(p))
        return -EINVAL;
    __builtin_memcpy(buf, &secret, sizeof(secret));
    __builtin_memcpy(buf + sizeof(secret), &p->key_size, sizeof(p->key_size));
    if (p->key_size)
        __builtin_memcpy(buf + ECDH_KPP_SECRET_MIN_SIZE, p->key, p->key_size);
    return 0;
}

static inline int crypto_ecdh_decode_key(const char *buf, unsigned int len, struct ecdh *p)
{
    struct kpp_secret secret;
    if (!buf || len < ECDH_KPP_SECRET_MIN_SIZE)
        return -EINVAL;
    __builtin_memcpy(&secret, buf, sizeof(secret));
    if (secret.type != CRYPTO_KPP_SECRET_TYPE_ECDH)
        return -EINVAL;
    __builtin_memcpy(&p->key_size, buf + sizeof(secret), sizeof(p->key_size));
    if (secret.len != crypto_ecdh_key_len(p))
        return -EINVAL;
    p->key = (char *)buf + ECDH_KPP_SECRET_MIN_SIZE;
    return 0;
}

#endif
