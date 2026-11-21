// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI: key agreement - ECDH on NIST P-256 ("ecdh-nist-p256"), what
 * Bluetooth LE Secure Connections pairing uses. The steps are those of
 * Linux's crypto/ecdh.c, on Linux's crypto/ecc.c (lib/crypto/ecc/),
 * without the asynchronous crypto framework around them.
 */
#include <linux/lkpi_all.h>
#include <linux/scatterlist.h>
#include <crypto/kpp.h>
#include <crypto/ecdh.h>
#include <crypto/internal/ecc.h>

struct crypto_kpp *crypto_alloc_kpp(const char *alg_name, u32 type, u32 mask)
{
	struct crypto_kpp *tfm;
	if (strcmp(alg_name, "ecdh-nist-p256") && strcmp(alg_name, "ecdh"))
		return ERR_PTR(-ENOENT);
	tfm = kzalloc(sizeof(*tfm), GFP_KERNEL);
	if (!tfm)
		return ERR_PTR(-ENOMEM);
	tfm->curve_id = ECC_CURVE_NIST_P256;
	tfm->ndigits = 4;
	return tfm;
}

void crypto_free_kpp(struct crypto_kpp *tfm)
{
	if (!IS_ERR_OR_NULL(tfm))
		kfree_sensitive(tfm);
}

int crypto_kpp_set_secret(struct crypto_kpp *tfm, const void *buf, unsigned int len)
{
	struct ecdh params;
	if (crypto_ecdh_decode_key(buf, len, &params) < 0 || params.key_size > sizeof(u64) * tfm->ndigits)
		return -EINVAL;
	memset(tfm->private_key, 0, sizeof(tfm->private_key));
	if (!params.key || !params.key_size)
		return ecc_gen_privkey(tfm->curve_id, tfm->ndigits, tfm->private_key);
	ecc_digits_from_bytes((const u8 *)params.key, params.key_size, tfm->private_key, tfm->ndigits);
	if (ecc_is_key_valid(tfm->curve_id, tfm->ndigits, tfm->private_key, params.key_size) < 0) {
		memset(tfm->private_key, 0, sizeof(tfm->private_key));
		return -EINVAL;
	}
	return 0;
}

static int copy_out(struct kpp_request *req, const void *buf, unsigned int nbytes)
{
	if (!req->dst || req->dst_len < nbytes)
		return -EINVAL;
	memcpy(sg_virt(req->dst), buf, nbytes);
	return 0;
}

int crypto_kpp_generate_public_key(struct kpp_request *req)
{
	struct crypto_kpp *tfm = req->tfm;
	u64 public_key[8];
	unsigned int nbytes = tfm->ndigits * sizeof(u64) * 2;
	int err = ecc_make_pub_key(tfm->curve_id, tfm->ndigits, tfm->private_key, public_key);
	if (!err)
		err = copy_out(req, public_key, nbytes);
	memzero_explicit(public_key, sizeof(public_key));
	return err;
}

int crypto_kpp_compute_shared_secret(struct kpp_request *req)
{
	struct crypto_kpp *tfm = req->tfm;
	u64 public_key[8], shared[4];
	unsigned int pub_size = tfm->ndigits * sizeof(u64) * 2;
	int err;
	if (!req->src || req->src_len < pub_size)
		return -EINVAL;
	memcpy(public_key, sg_virt(req->src), pub_size);
	err = crypto_ecdh_shared_secret(tfm->curve_id, tfm->ndigits, tfm->private_key, public_key, shared);
	if (!err)
		err = copy_out(req, shared, tfm->ndigits * sizeof(u64) < req->dst_len ? tfm->ndigits * sizeof(u64) : req->dst_len);
	memzero_explicit(shared, sizeof(shared));
	return err;
}

/* include/linux/efi.h: no runtime services reachable. */
#include <linux/efi.h>
struct lkpi_efi efi;
