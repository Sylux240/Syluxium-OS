// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI: the synchronous crypto transforms mac80211 uses for software
 * encryption (when the driver doesn't offload it): AES-CCM (CCMP), AES-GCM
 * (GCMP, GMAC), AES-CMAC (BIP), AES-CTR (FILS). Built on Linux's generic
 * AES (lib/crypto/aes.c). Scatterlists are linearized into a bounce buffer.
 */
#include <linux/lkpi_all.h>
#include <crypto/aes.h>
#include <crypto/aead.h>
#include <crypto/hash.h>
#include <crypto/skcipher.h>
#include <crypto/utils.h>

enum { LKPI_ALG_CCM = 1, LKPI_ALG_GCM, LKPI_ALG_CMAC, LKPI_ALG_CTR };

static int alg_of(const char* name)
{
	if (!strcmp(name, "ccm(aes)"))
		return LKPI_ALG_CCM;
	if (!strcmp(name, "gcm(aes)") || !strcmp(name, "gmac(aes)"))
		return LKPI_ALG_GCM;
	if (!strcmp(name, "cmac(aes)"))
		return LKPI_ALG_CMAC;
	if (!strcmp(name, "ctr(aes)"))
		return LKPI_ALG_CTR;
	return 0;
}

static void* crypto_new(const char* name, size_t size)
{
	struct lkpi_crypto* c;
	int alg = alg_of(name);
	if (!alg) {
		printk(KERN_WARNING "linuxkpi: crypto algorithm %s not available\n", name);
		return ERR_PTR(-ENOENT);
	}
	c = kzalloc(size, GFP_KERNEL);
	if (!c)
		return ERR_PTR(-ENOMEM);
	c->aes = kzalloc(sizeof(struct crypto_aes_ctx), GFP_KERNEL);
	if (!c->aes) {
		kfree(c);
		return ERR_PTR(-ENOMEM);
	}
	c->alg = alg;
	c->authsize = 16;
	return c;
}

static void crypto_del(struct lkpi_crypto* c)
{
	if (IS_ERR_OR_NULL(c))
		return;
	kfree_sensitive(c->aes);
	kfree_sensitive(c);
}

static int crypto_key(struct lkpi_crypto* c, const u8* key, unsigned int keylen)
{
	if (keylen > sizeof(c->key))
		return -EINVAL;
	memcpy(c->key, key, keylen);
	c->keylen = keylen;
	return aes_expandkey(c->aes, key, keylen);
}

/* ---- scatterlist helpers ---- */

static unsigned int sg_copy_out(struct scatterlist* sg, u8* buf, unsigned int skip, unsigned int len)
{
	unsigned int done = 0;
	for (; sg && done < len; sg = sg_next(sg)) {
		unsigned int n = sg->length;
		const u8* p = sg_virt(sg);
		if (skip >= n) {
			skip -= n;
			continue;
		}
		p += skip;
		n -= skip;
		skip = 0;
		if (n > len - done)
			n = len - done;
		memcpy(buf + done, p, n);
		done += n;
	}
	return done;
}

static unsigned int sg_copy_in(struct scatterlist* sg, const u8* buf, unsigned int skip, unsigned int len)
{
	unsigned int done = 0;
	for (; sg && done < len; sg = sg_next(sg)) {
		unsigned int n = sg->length;
		u8* p = sg_virt(sg);
		if (skip >= n) {
			skip -= n;
			continue;
		}
		p += skip;
		n -= skip;
		skip = 0;
		if (n > len - done)
			n = len - done;
		memcpy(p, buf + done, n);
		done += n;
	}
	return done;
}

/* ---- CCM (RFC 3610): iv[0] = L - 1, nonce = iv[1 .. 15 - L] ---- */

static void ccm_crypt(const struct crypto_aes_ctx* aes, const u8* iv, u8* data, unsigned int len, u8* s0)
{
	u8 ctr[16], stream[16];
	unsigned int L = iv[0] + 1, i;
	memcpy(ctr, iv, 16);
	memset(ctr + 16 - L, 0, L);
	aes_encrypt(aes, s0, ctr); /* counter 0: encrypts the MIC */
	for (unsigned int off = 0; off < len; off += 16) {
		/* increment the L-byte counter */
		for (i = 15; i >= 16 - L; i--)
			if (++ctr[i])
				break;
		aes_encrypt(aes, stream, ctr);
		crypto_xor(data + off, stream, min(16u, len - off));
	}
}

static void ccm_mac(const struct crypto_aes_ctx* aes, const u8* iv, unsigned int authsize, const u8* aad,
		    unsigned int aadlen, const u8* msg, unsigned int len, u8* mac)
{
	u8 b[16], x[16];
	unsigned int L = iv[0] + 1, i, off;
	/* B0: flags | nonce | message length */
	memcpy(b, iv, 16);
	b[0] = (aadlen ? 0x40 : 0) | (((authsize - 2) / 2) << 3) | (L - 1);
	for (i = 0; i < L; i++)
		b[15 - i] = (len >> (8 * i)) & 0xff;
	aes_encrypt(aes, x, b);
	if (aadlen) {
		unsigned int pos = 2;
		memset(b, 0, 16);
		b[0] = aadlen >> 8;
		b[1] = aadlen & 0xff;
		for (off = 0; off < aadlen;) {
			unsigned int n = min(16 - pos, aadlen - off);
			memcpy(b + pos, aad + off, n);
			off += n;
			pos += n;
			if (pos == 16 || off == aadlen) {
				crypto_xor(x, b, 16);
				aes_encrypt(aes, x, x);
				memset(b, 0, 16);
				pos = 0;
			}
		}
	}
	for (off = 0; off < len; off += 16) {
		memset(b, 0, 16);
		memcpy(b, msg + off, min(16u, len - off));
		crypto_xor(x, b, 16);
		aes_encrypt(aes, x, x);
	}
	memcpy(mac, x, authsize);
}

/* ---- GCM ---- */

static void gf128_mul(u8* x, const u8* h)
{
	u8 z[16] = {0}, v[16];
	memcpy(v, h, 16);
	for (int i = 0; i < 128; i++) {
		if (x[i / 8] & (0x80 >> (i % 8)))
			crypto_xor(z, v, 16);
		bool lsb = v[15] & 1;
		for (int j = 15; j > 0; j--)
			v[j] = (v[j] >> 1) | (v[j - 1] << 7);
		v[0] >>= 1;
		if (lsb)
			v[0] ^= 0xe1;
	}
	memcpy(x, z, 16);
}

static void ghash(const u8* h, u8* y, const u8* data, unsigned int len)
{
	for (unsigned int off = 0; off < len; off += 16) {
		u8 b[16] = {0};
		memcpy(b, data + off, min(16u, len - off));
		crypto_xor(y, b, 16);
		gf128_mul(y, h);
	}
}

static void gcm_tag(const struct crypto_aes_ctx* aes, const u8* j0, const u8* aad, unsigned int aadlen,
		    const u8* ct, unsigned int len, u8* tag)
{
	u8 h[16] = {0}, y[16] = {0}, lens[16], ek[16];
	aes_encrypt(aes, h, h);
	ghash(h, y, aad, aadlen);
	ghash(h, y, ct, len);
	put_unaligned_be64((u64)aadlen * 8, lens);
	put_unaligned_be64((u64)len * 8, lens + 8);
	crypto_xor(y, lens, 16);
	gf128_mul(y, h);
	aes_encrypt(aes, ek, j0);
	crypto_xor_cpy(tag, y, ek, 16);
}

static void gcm_ctr(const struct crypto_aes_ctx* aes, const u8* j0, u8* data, unsigned int len)
{
	u8 ctr[16], stream[16];
	memcpy(ctr, j0, 16);
	for (unsigned int off = 0; off < len; off += 16) {
		u32 c = get_unaligned_be32(ctr + 12) + 1;
		put_unaligned_be32(c, ctr + 12);
		aes_encrypt(aes, stream, ctr);
		crypto_xor(data + off, stream, min(16u, len - off));
	}
}

/* ---- AEAD ---- */

struct crypto_aead* crypto_alloc_aead(const char* alg_name, u32 type, u32 mask)
{
	struct crypto_aead* t = crypto_new(alg_name, sizeof(*t));
	if (!IS_ERR(t) && t->c.alg != LKPI_ALG_CCM && t->c.alg != LKPI_ALG_GCM) {
		crypto_del(&t->c);
		return ERR_PTR(-ENOENT);
	}
	return t;
}

void crypto_free_aead(struct crypto_aead* tfm) { crypto_del((struct lkpi_crypto*)tfm); }
int crypto_aead_setkey(struct crypto_aead* tfm, const u8* key, unsigned int keylen) { return crypto_key(&tfm->c, key, keylen); }

int crypto_aead_setauthsize(struct crypto_aead* tfm, unsigned int authsize)
{
	if (authsize < 4 || authsize > 16)
		return -EINVAL;
	tfm->c.authsize = authsize;
	return 0;
}

static int aead_crypt(struct aead_request* req, bool enc)
{
	struct lkpi_crypto* c = &req->tfm->c;
	unsigned int as = c->authsize, assoclen = req->assoclen;
	unsigned int len = enc ? req->cryptlen : req->cryptlen - as;
	unsigned int total = assoclen + req->cryptlen + (enc ? as : 0);
	u8 tag[16], s0[16], j0[16];
	u8* buf;
	int ret = 0;

	if (!enc && req->cryptlen < as)
		return -EINVAL;
	buf = kmalloc(total + 16, GFP_ATOMIC);
	if (!buf)
		return -ENOMEM;
	sg_copy_out(req->src, buf, 0, assoclen + req->cryptlen);
	u8* aad = buf;
	u8* msg = buf + assoclen;

	if (c->alg == LKPI_ALG_CCM) {
		if (enc) {
			ccm_mac(c->aes, req->iv, as, aad, assoclen, msg, len, tag);
			ccm_crypt(c->aes, req->iv, msg, len, s0);
			crypto_xor_cpy(msg + len, tag, s0, as);
		} else {
			ccm_crypt(c->aes, req->iv, msg, len, s0);
			ccm_mac(c->aes, req->iv, as, aad, assoclen, msg, len, tag);
			crypto_xor(tag, s0, as);
			if (crypto_memneq(tag, msg + len, as))
				ret = -EBADMSG;
		}
	} else {
		memcpy(j0, req->iv, 12);
		put_unaligned_be32(1, j0 + 12);
		if (enc) {
			gcm_ctr(c->aes, j0, msg, len);
			gcm_tag(c->aes, j0, aad, assoclen, msg, len, tag);
			memcpy(msg + len, tag, as);
		} else {
			gcm_tag(c->aes, j0, aad, assoclen, msg, len, tag);
			if (crypto_memneq(tag, msg + len, as))
				ret = -EBADMSG;
			else
				gcm_ctr(c->aes, j0, msg, len);
		}
	}
	if (!ret)
		sg_copy_in(req->dst, buf, 0, assoclen + len + (enc ? as : 0));
	kfree_sensitive(buf);
	return ret;
}

int crypto_aead_encrypt(struct aead_request* req) { return aead_crypt(req, true); }
int crypto_aead_decrypt(struct aead_request* req) { return aead_crypt(req, false); }

/* ---- CMAC (RFC 4493) ---- */

struct crypto_shash* crypto_alloc_shash(const char* alg_name, u32 type, u32 mask)
{
	struct crypto_shash* t = crypto_new(alg_name, sizeof(*t));
	if (!IS_ERR(t) && t->c.alg != LKPI_ALG_CMAC) {
		crypto_del(&t->c);
		return ERR_PTR(-ENOENT);
	}
	return t;
}

void crypto_free_shash(struct crypto_shash* tfm) { crypto_del((struct lkpi_crypto*)tfm); }
int crypto_shash_setkey(struct crypto_shash* tfm, const u8* key, unsigned int keylen) { return crypto_key(&tfm->c, key, keylen); }

int crypto_shash_init(struct shash_desc* desc)
{
	memset(desc->lkpi_x, 0, 16);
	desc->lkpi_buflen = 0;
	return 0;
}

int crypto_shash_update(struct shash_desc* desc, const u8* data, unsigned int len)
{
	const struct crypto_aes_ctx* aes = desc->tfm->c.aes;
	while (len) {
		/* Keep the last (possibly full) block buffered for final(). */
		if (desc->lkpi_buflen == 16) {
			crypto_xor(desc->lkpi_x, desc->lkpi_buf, 16);
			aes_encrypt(aes, desc->lkpi_x, desc->lkpi_x);
			desc->lkpi_buflen = 0;
		}
		unsigned int n = min(16 - desc->lkpi_buflen, len);
		memcpy(desc->lkpi_buf + desc->lkpi_buflen, data, n);
		desc->lkpi_buflen += n;
		data += n;
		len -= n;
	}
	return 0;
}

static void cmac_dbl(u8* k)
{
	u8 carry = k[0] & 0x80;
	for (int i = 0; i < 15; i++)
		k[i] = (k[i] << 1) | (k[i + 1] >> 7);
	k[15] <<= 1;
	if (carry)
		k[15] ^= 0x87;
}

int crypto_shash_final(struct shash_desc* desc, u8* out)
{
	const struct crypto_aes_ctx* aes = desc->tfm->c.aes;
	u8 k[16] = {0}, last[16];
	aes_encrypt(aes, k, k);
	cmac_dbl(k); /* K1 */
	if (desc->lkpi_buflen == 16) {
		crypto_xor_cpy(last, desc->lkpi_buf, k, 16);
	} else {
		cmac_dbl(k); /* K2 */
		memset(last, 0, 16);
		memcpy(last, desc->lkpi_buf, desc->lkpi_buflen);
		last[desc->lkpi_buflen] = 0x80;
		crypto_xor(last, k, 16);
	}
	crypto_xor(desc->lkpi_x, last, 16);
	aes_encrypt(aes, desc->lkpi_x, desc->lkpi_x);
	memcpy(out, desc->lkpi_x, 16);
	return 0;
}

int crypto_shash_finup(struct shash_desc* desc, const u8* data, unsigned int len, u8* out)
{
	crypto_shash_update(desc, data, len);
	return crypto_shash_final(desc, out);
}

int crypto_shash_digest(struct shash_desc* desc, const u8* data, unsigned int len, u8* out)
{
	crypto_shash_init(desc);
	return crypto_shash_finup(desc, data, len, out);
}

/* ---- CTR ---- */

struct crypto_skcipher* crypto_alloc_skcipher(const char* alg_name, u32 type, u32 mask)
{
	struct crypto_skcipher* t = crypto_new(alg_name, sizeof(*t));
	if (!IS_ERR(t) && t->c.alg != LKPI_ALG_CTR) {
		crypto_del(&t->c);
		return ERR_PTR(-ENOENT);
	}
	return t;
}

void crypto_free_skcipher(struct crypto_skcipher* tfm) { crypto_del((struct lkpi_crypto*)tfm); }
int crypto_skcipher_setkey(struct crypto_skcipher* tfm, const u8* key, unsigned int keylen) { return crypto_key(&tfm->c, key, keylen); }

static int ctr_crypt(struct skcipher_request* req)
{
	const struct crypto_aes_ctx* aes = req->tfm->c.aes;
	u8 ctr[16], stream[16];
	u8* buf = kmalloc(req->cryptlen + 16, GFP_ATOMIC);
	if (!buf)
		return -ENOMEM;
	sg_copy_out(req->src, buf, 0, req->cryptlen);
	memcpy(ctr, req->iv, 16);
	for (unsigned int off = 0; off < req->cryptlen; off += 16) {
		aes_encrypt(aes, stream, ctr);
		crypto_xor(buf + off, stream, min(16u, req->cryptlen - off));
		for (int i = 15; i >= 0; i--)
			if (++ctr[i])
				break;
	}
	sg_copy_in(req->dst, buf, 0, req->cryptlen);
	kfree_sensitive(buf);
	return 0;
}

int crypto_skcipher_encrypt(struct skcipher_request* req) { return ctr_crypt(req); }
int crypto_skcipher_decrypt(struct skcipher_request* req) { return ctr_crypt(req); }
