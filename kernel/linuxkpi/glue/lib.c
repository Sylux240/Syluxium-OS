#include <linux/uaccess.h>
#include <linux/io.h>
// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI: small library pieces the wireless stack needs - ctype, string
 * and number parsing, ID allocators, hash tables, high-resolution timers,
 * the device core (no sysfs), platform devices, random numbers.
 */
#include <linux/lkpi_all.h>
#include <linux/ctype.h>
#include <linux/gcd.h>
#include <linux/idr.h>
#include <linux/rhashtable.h>
#include <linux/hrtimer.h>
#include <linux/platform_device.h>
#include <linux/utsname.h>
#include <linux/random.h>

/* ---- ctype (Linux's table) ---- */
const unsigned char _ctype[] = {
	_C, _C, _C, _C, _C, _C, _C, _C, _C, _C | _S, _C | _S, _C | _S, _C | _S, _C | _S, _C, _C,
	_C, _C, _C, _C, _C, _C, _C, _C, _C, _C, _C, _C, _C, _C, _C, _C,
	_S | _SP, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P,
	_D, _D, _D, _D, _D, _D, _D, _D, _D, _D, _P, _P, _P, _P, _P, _P,
	_P, _U | _X, _U | _X, _U | _X, _U | _X, _U | _X, _U | _X, _U, _U, _U, _U, _U, _U, _U, _U, _U,
	_U, _U, _U, _U, _U, _U, _U, _U, _U, _U, _U, _P, _P, _P, _P, _P,
	_P, _L | _X, _L | _X, _L | _X, _L | _X, _L | _X, _L | _X, _L, _L, _L, _L, _L, _L, _L, _L, _L,
	_L, _L, _L, _L, _L, _L, _L, _L, _L, _L, _L, _P, _P, _P, _P, _C,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	_S | _SP, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P,
	_P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P, _P,
	_U, _U, _U, _U, _U, _U, _U, _U, _U, _U, _U, _U, _U, _U, _U, _U,
	_U, _U, _U, _U, _U, _U, _U, _P, _U, _U, _U, _U, _U, _U, _U, _L,
	_L, _L, _L, _L, _L, _L, _L, _L, _L, _L, _L, _L, _L, _L, _L, _L,
	_L, _L, _L, _L, _L, _L, _L, _P, _L, _L, _L, _L, _L, _L, _L, _L,
};

unsigned long gcd(unsigned long a, unsigned long b)
{
	while (b) {
		unsigned long t = a % b;
		a = b;
		b = t;
	}
	return a;
}

unsigned long lkpi_int_sqrt(unsigned long x)
{
	unsigned long r = 0, bit = 1UL << (BITS_PER_LONG - 2);
	while (bit > x)
		bit >>= 2;
	while (bit) {
		if (x >= r + bit) {
			x -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return r;
}

/* ---- strings ---- */

const char hex_asc[] = "0123456789abcdef";

int hex_to_bin(unsigned char ch)
{
	if (ch >= '0' && ch <= '9')
		return ch - '0';
	ch = tolower(ch);
	if (ch >= 'a' && ch <= 'f')
		return ch - 'a' + 10;
	return -1;
}

int hex2bin(u8* dst, const char* src, size_t count)
{
	while (count--) {
		int hi = hex_to_bin(*src++), lo;
		if (hi < 0)
			return -EINVAL;
		lo = hex_to_bin(*src++);
		if (lo < 0)
			return -EINVAL;
		*dst++ = (hi << 4) | lo;
	}
	return 0;
}

char* bin2hex(char* dst, const void* src, size_t count)
{
	const unsigned char* _src = src;
	while (count--) {
		*dst++ = hex_asc_hi(*_src);
		*dst++ = hex_asc_lo(*_src++);
	}
	return dst;
}

char* kstrndup(const char* s, size_t max, gfp_t gfp)
{
	size_t len;
	char* buf;
	if (!s)
		return NULL;
	len = strnlen(s, max);
	buf = kmalloc(len + 1, gfp);
	if (buf) {
		memcpy(buf, s, len);
		buf[len] = 0;
	}
	return buf;
}

int strncasecmp(const char* a, const char* b, size_t n)
{
	while (n--) {
		int ca = tolower((unsigned char)*a++), cb = tolower((unsigned char)*b++);
		if (ca != cb || !ca)
			return ca - cb;
	}
	return 0;
}

size_t strspn(const char* s, const char* accept)
{
	const char* p;
	for (p = s; *p && strchr(accept, *p); p++)
		;
	return p - s;
}

size_t strcspn(const char* s, const char* reject)
{
	const char* p;
	for (p = s; *p && !strchr(reject, *p); p++)
		;
	return p - s;
}

char* strpbrk(const char* cs, const char* ct)
{
	for (; *cs; cs++)
		if (strchr(ct, *cs))
			return (char*)cs;
	return NULL;
}

char* strsep(char** s, const char* ct)
{
	char *sbegin = *s, *end;
	if (!sbegin)
		return NULL;
	end = strpbrk(sbegin, ct);
	if (end)
		*end++ = '\0';
	*s = end;
	return sbegin;
}

char* skip_spaces(const char* str)
{
	while (isspace(*str))
		++str;
	return (char*)str;
}

char* strim(char* s)
{
	size_t size = strlen(s);
	char* end;
	if (!size)
		return s;
	end = s + size - 1;
	while (end >= s && isspace(*end))
		end--;
	*(end + 1) = '\0';
	return skip_spaces(s);
}

size_t strlcat(char* dest, const char* src, size_t count)
{
	size_t dsize = strlen(dest), len = strlen(src), res = dsize + len;
	if (dsize >= count)
		return res;
	dest += dsize;
	count -= dsize;
	if (len >= count)
		len = count - 1;
	memcpy(dest, src, len);
	dest[len] = 0;
	return res;
}

char* strnstr(const char* s1, const char* s2, size_t len)
{
	size_t l2 = strlen(s2);
	if (!l2)
		return (char*)s1;
	while (len >= l2) {
		len--;
		if (!memcmp(s1, s2, l2))
			return (char*)s1;
		s1++;
	}
	return NULL;
}

char* strreplace(char* s, char old, char n)
{
	for (; *s; ++s)
		if (*s == old)
			*s = n;
	return s;
}

void* memscan(void* addr, int c, size_t size)
{
	unsigned char* p = addr;
	while (size) {
		if (*p == (unsigned char)c)
			return p;
		p++;
		size--;
	}
	return p;
}

/* ---- number parsing ---- */

int kstrtoull(const char* s, unsigned int base, unsigned long long* res)
{
	unsigned long long v = 0;
	int any = 0;
	if (*s == '+')
		s++;
	if (base == 0) {
		if (s[0] == '0' && tolower(s[1]) == 'x' && isxdigit(s[2])) {
			base = 16;
			s += 2;
		} else if (s[0] == '0') {
			base = 8;
		} else {
			base = 10;
		}
	} else if (base == 16 && s[0] == '0' && tolower(s[1]) == 'x') {
		s += 2;
	}
	for (; *s; s++) {
		int d = hex_to_bin(*s);
		if (d < 0 || d >= (int)base)
			break;
		v = v * base + d;
		any = 1;
	}
	if (*s == '\n')
		s++;
	if (!any || *s)
		return -EINVAL;
	*res = v;
	return 0;
}

int kstrtoll(const char* s, unsigned int base, long long* res)
{
	unsigned long long v;
	int neg = *s == '-', r;
	r = kstrtoull(s + neg, base, &v);
	if (r)
		return r;
	*res = neg ? -(long long)v : (long long)v;
	return 0;
}

int kstrtol(const char* s, unsigned int base, long* res)
{
	long long v;
	int r = kstrtoll(s, base, &v);
	if (!r)
		*res = v;
	return r;
}

#define KSTRTO(name, type, max)                                                  \
	int name(const char* s, unsigned int base, type* res)                    \
	{                                                                        \
		unsigned long long v;                                            \
		int r = kstrtoull(s, base, &v);                                  \
		if (r)                                                           \
			return r;                                                \
		if (v > (max))                                                   \
			return -ERANGE;                                          \
		*res = (type)v;                                                  \
		return 0;                                                        \
	}
KSTRTO(lkpi_kstrtou8, u8, 0xff)
KSTRTO(kstrtou16, u16, 0xffff)
KSTRTO(kstrtou32, u32, 0xffffffffu)

int kstrtos32(const char* s, unsigned int base, s32* res)
{
	long long v;
	int r = kstrtoll(s, base, &v);
	if (r)
		return r;
	if (v > S32_MAX || v < S32_MIN)
		return -ERANGE;
	*res = (s32)v;
	return 0;
}

int kstrtobool(const char* s, bool* res)
{
	if (!s)
		return -EINVAL;
	switch (s[0]) {
	case 'y': case 'Y': case 't': case 'T': case '1':
		*res = true;
		return 0;
	case 'n': case 'N': case 'f': case 'F': case '0':
		*res = false;
		return 0;
	case 'o': case 'O':
		if (s[1] == 'n' || s[1] == 'N') { *res = true; return 0; }
		if (s[1] == 'f' || s[1] == 'F') { *res = false; return 0; }
		break;
	}
	return -EINVAL;
}

/* A small sscanf: %d %i %u %x %hhx %hx %s %c (what drivers use for MACs). */
int sscanf(const char* buf, const char* fmt, ...)
{
	va_list ap;
	int count = 0;
	va_start(ap, fmt);
	while (*fmt) {
		if (isspace(*fmt)) {
			buf = skip_spaces(buf);
			fmt++;
			continue;
		}
		if (*fmt != '%') {
			if (*buf++ != *fmt++)
				break;
			continue;
		}
		fmt++;
		int qual = 0;
		while (*fmt == 'h' || *fmt == 'l') {
			qual = qual * 2 + (*fmt == 'h' ? 1 : 2);
			fmt++;
		}
		char conv = *fmt++;
		if (conv == 's') {
			char* out = va_arg(ap, char*);
			buf = skip_spaces(buf);
			if (!*buf)
				break;
			while (*buf && !isspace(*buf))
				*out++ = *buf++;
			*out = 0;
			count++;
			continue;
		}
		if (conv == 'c') {
			if (!*buf)
				break;
			*va_arg(ap, char*) = *buf++;
			count++;
			continue;
		}
		unsigned int base = conv == 'x' || conv == 'X' ? 16 : conv == 'i' ? 0 : 10;
		bool neg = false;
		if (conv != 'x' && conv != 'X' && conv != 'u')
			if (*buf == '-') { neg = true; buf++; }
		if (base == 0) {
			if (buf[0] == '0' && tolower(buf[1]) == 'x') { base = 16; buf += 2; }
			else base = 10;
		}
		unsigned long long v = 0;
		int any = 0;
		for (;; buf++) {
			int d = hex_to_bin(*buf);
			if (d < 0 || d >= (int)base)
				break;
			v = v * base + d;
			any = 1;
		}
		if (!any)
			break;
		if (neg)
			v = -v;
		if (qual == 3)        /* hh */
			*va_arg(ap, unsigned char*) = (unsigned char)v;
		else if (qual == 1)   /* h */
			*va_arg(ap, unsigned short*) = (unsigned short)v;
		else if (qual >= 2)   /* l, ll */
			*va_arg(ap, unsigned long*) = (unsigned long)v;
		else
			*va_arg(ap, unsigned int*) = (unsigned int)v;
		count++;
	}
	va_end(ap);
	return count;
}

int param_set_uint(const char* val, const struct kernel_param* kp) { return kstrtouint(val, 0, kp->arg); }
int param_get_uint(char* buffer, const struct kernel_param* kp) { return sprintf(buffer, "%u\n", *(unsigned int*)kp->arg); }
int param_set_int(const char* val, const struct kernel_param* kp) { return kstrtoint(val, 0, kp->arg); }
int param_get_int(char* buffer, const struct kernel_param* kp) { return sprintf(buffer, "%d\n", *(int*)kp->arg); }
int param_set_bool(const char* val, const struct kernel_param* kp) { return kstrtobool(val, kp->arg); }
int param_get_bool(char* buffer, const struct kernel_param* kp) { return sprintf(buffer, "%c\n", *(bool*)kp->arg ? 'Y' : 'N'); }

/* ---- misc ---- */

static struct new_utsname utsname_data = {
	.sysname = "Linux", .nodename = "syluxium", .release = "6.12.0-syluxium", .version = "#1 SMP",
	.machine = "x86_64", .domainname = "(none)",
};
struct new_utsname* init_utsname(void) { return &utsname_data; }

void do_trace_netlink_extack(const char* msg) {}

unsigned long* bitmap_zalloc(unsigned int nbits, gfp_t flags)
{
	return kcalloc(BITS_TO_LONGS(nbits), sizeof(unsigned long), flags);
}

unsigned long* bitmap_alloc(unsigned int nbits, gfp_t flags)
{
	return kmalloc_array(BITS_TO_LONGS(nbits), sizeof(unsigned long), flags);
}

void bitmap_free(const unsigned long* bitmap) { kfree(bitmap); }

/* ---- idr / ida: a sorted array of (id, pointer) ---- */

static int idr_slot(const struct idr* idr, unsigned long id)
{
	unsigned int lo = 0, hi = idr->count;
	while (lo < hi) {
		unsigned int mid = (lo + hi) / 2;
		if (idr->entries[mid].id < id)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static int idr_insert(struct idr* idr, unsigned long id, void* ptr, gfp_t gfp)
{
	int pos = idr_slot(idr, id);
	if (idr->count == idr->capacity) {
		unsigned int cap = idr->capacity ? idr->capacity * 2 : 8;
		struct lkpi_idr_entry* n = krealloc(idr->entries, cap * sizeof(*n), gfp);
		if (!n)
			return -ENOMEM;
		idr->entries = n;
		idr->capacity = cap;
	}
	memmove(&idr->entries[pos + 1], &idr->entries[pos], (idr->count - pos) * sizeof(idr->entries[0]));
	idr->entries[pos].id = id;
	idr->entries[pos].ptr = ptr;
	idr->count++;
	return 0;
}

static int idr_alloc_from(struct idr* idr, void* ptr, unsigned long start, unsigned long end, gfp_t gfp)
{
	unsigned long id = start;
	unsigned long flags;
	int pos, r;
	spin_lock_irqsave(&idr->lock, flags);
	for (pos = idr_slot(idr, id); pos < (int)idr->count && idr->entries[pos].id == id; pos++)
		id++;
	if (id >= end) {
		spin_unlock_irqrestore(&idr->lock, flags);
		return -ENOSPC;
	}
	r = idr_insert(idr, id, ptr, GFP_ATOMIC);
	spin_unlock_irqrestore(&idr->lock, flags);
	return r ? r : (int)id;
}

int idr_alloc(struct idr* idr, void* ptr, int start, int end, gfp_t gfp)
{
	if (start < (int)idr->idr_base)
		start = idr->idr_base;
	return idr_alloc_from(idr, ptr, start, end <= 0 ? 0x80000000UL : (unsigned long)end, gfp);
}

int idr_alloc_cyclic(struct idr* idr, void* ptr, int start, int end, gfp_t gfp)
{
	int id = idr_alloc(idr, ptr, max_t(int, start, idr->idr_next), end, gfp);
	if (id == -ENOSPC)
		id = idr_alloc(idr, ptr, start, end, gfp);
	if (id >= 0)
		idr->idr_next = id + 1;
	return id;
}

void* idr_find(const struct idr* idr, unsigned long id)
{
	int pos = idr_slot(idr, id);
	return pos < (int)idr->count && idr->entries[pos].id == id ? idr->entries[pos].ptr : NULL;
}

void* idr_remove(struct idr* idr, unsigned long id)
{
	unsigned long flags;
	void* ptr = NULL;
	int pos;
	spin_lock_irqsave(&idr->lock, flags);
	pos = idr_slot(idr, id);
	if (pos < (int)idr->count && idr->entries[pos].id == id) {
		ptr = idr->entries[pos].ptr;
		memmove(&idr->entries[pos], &idr->entries[pos + 1], (idr->count - pos - 1) * sizeof(idr->entries[0]));
		idr->count--;
	}
	spin_unlock_irqrestore(&idr->lock, flags);
	return ptr;
}

void* idr_replace(struct idr* idr, void* ptr, unsigned long id)
{
	int pos = idr_slot(idr, id);
	void* old;
	if (pos >= (int)idr->count || idr->entries[pos].id != id)
		return ERR_PTR(-ENOENT);
	old = idr->entries[pos].ptr;
	idr->entries[pos].ptr = ptr;
	return old;
}

void* idr_get_next_ul(struct idr* idr, unsigned long* nextid)
{
	int pos = idr_slot(idr, *nextid);
	for (; pos < (int)idr->count; pos++) {
		if (idr->entries[pos].ptr) {
			*nextid = idr->entries[pos].id;
			return idr->entries[pos].ptr;
		}
	}
	return NULL;
}

void* idr_get_next(struct idr* idr, int* nextid)
{
	unsigned long id = *nextid;
	void* p = idr_get_next_ul(idr, &id);
	if (p)
		*nextid = (int)id;
	return p;
}

int idr_for_each(const struct idr* idr, int (*fn)(int id, void* p, void* data), void* data)
{
	for (unsigned int i = 0; i < idr->count; i++) {
		int r = fn((int)idr->entries[i].id, idr->entries[i].ptr, data);
		if (r)
			return r;
	}
	return 0;
}

void idr_destroy(struct idr* idr)
{
	kfree(idr->entries);
	idr->entries = NULL;
	idr->count = idr->capacity = 0;
}

int ida_alloc_range(struct ida* ida, unsigned int min, unsigned int max, gfp_t gfp)
{
	return idr_alloc_from(&ida->idr, (void*)1, min, (unsigned long)max + 1, gfp);
}

void ida_free(struct ida* ida, unsigned int id)
{
	idr_remove(&ida->idr, id);
}

/* ---- rhashtable (fixed buckets, see linux/rhashtable.h) ---- */

static u32 rht_hash(struct rhashtable* ht, const void* key)
{
	u32 h = ht->p.hashfn ? ht->p.hashfn(key, ht->p.key_len, 0) : jhash(key, ht->p.key_len, 0);
	return h % LKPI_RHT_BUCKETS;
}

static const void* rht_key(struct rhashtable* ht, const struct rhash_head* he)
{
	return (const char*)rht_obj(ht, he) + ht->p.key_offset;
}

static bool rht_match(struct rhashtable* ht, const struct rhash_head* he, const void* key)
{
	if (ht->p.obj_cmpfn) {
		struct rhashtable_compare_arg arg = {.ht = ht, .key = key};
		return ht->p.obj_cmpfn(&arg, rht_obj(ht, he)) == 0;
	}
	return !memcmp(rht_key(ht, he), key, ht->p.key_len);
}

int rhashtable_init(struct rhashtable* ht, const struct rhashtable_params* params)
{
	memset(ht, 0, sizeof(*ht));
	ht->p = *params;
	spin_lock_init(&ht->lock);
	return 0;
}

int rhltable_init(struct rhltable* hlt, const struct rhashtable_params* params)
{
	int r = rhashtable_init(&hlt->ht, params);
	hlt->ht.rhlist = true;
	return r;
}

void rhashtable_free_and_destroy(struct rhashtable* ht, void (*free_fn)(void* ptr, void* arg), void* arg)
{
	if (!free_fn)
		return;
	for (int b = 0; b < LKPI_RHT_BUCKETS; b++) {
		struct rhash_head* he = ht->buckets[b];
		while (he) {
			struct rhash_head* next = he->next;
			if (ht->rhlist) {
				struct rhlist_head* l = container_of(he, struct rhlist_head, rhead);
				while (l) {
					struct rhlist_head* ln = l->next;
					free_fn(rht_obj(ht, &l->rhead), arg);
					l = ln;
				}
			} else {
				free_fn(rht_obj(ht, he), arg);
			}
			he = next;
		}
		ht->buckets[b] = NULL;
	}
}

void rhashtable_destroy(struct rhashtable* ht)
{
	rhashtable_free_and_destroy(ht, NULL, NULL);
}

void* lkpi_rht_lookup(struct rhashtable* ht, const void* key)
{
	struct rhash_head* he;
	for (he = READ_ONCE(ht->buckets[rht_hash(ht, key)]); he; he = READ_ONCE(he->next))
		if (rht_match(ht, he, key))
			return rht_obj(ht, he);
	return NULL;
}

struct rhlist_head* lkpi_rhl_lookup(struct rhltable* hlt, const void* key)
{
	struct rhashtable* ht = &hlt->ht;
	struct rhash_head* he;
	for (he = READ_ONCE(ht->buckets[rht_hash(ht, key)]); he; he = READ_ONCE(he->next))
		if (rht_match(ht, he, key))
			return container_of(he, struct rhlist_head, rhead);
	return NULL;
}

static int rht_insert_locked(struct rhashtable* ht, struct rhash_head* obj, bool unique, void** existing)
{
	const void* key = rht_key(ht, obj);
	u32 b = rht_hash(ht, key);
	struct rhash_head* he;
	for (he = ht->buckets[b]; he; he = he->next) {
		if (rht_match(ht, he, key)) {
			if (existing)
				*existing = rht_obj(ht, he);
			if (unique)
				return -EEXIST;
		}
	}
	obj->next = ht->buckets[b];
	__atomic_store_n(&ht->buckets[b], obj, __ATOMIC_RELEASE);
	atomic_inc(&ht->nelems);
	return 0;
}

int lkpi_rht_insert(struct rhashtable* ht, struct rhash_head* obj, bool unique)
{
	unsigned long flags;
	int r;
	spin_lock_irqsave(&ht->lock, flags);
	r = rht_insert_locked(ht, obj, unique, NULL);
	spin_unlock_irqrestore(&ht->lock, flags);
	return r;
}

void* lkpi_rht_lookup_get_insert(struct rhashtable* ht, const void* key, struct rhash_head* obj)
{
	unsigned long flags;
	void* existing = NULL;
	spin_lock_irqsave(&ht->lock, flags);
	if (rht_insert_locked(ht, obj, true, &existing))
		;
	spin_unlock_irqrestore(&ht->lock, flags);
	return existing;
}

int lkpi_rht_remove(struct rhashtable* ht, struct rhash_head* obj)
{
	unsigned long flags;
	struct rhash_head** pp;
	int r = -ENOENT;
	spin_lock_irqsave(&ht->lock, flags);
	for (pp = &ht->buckets[rht_hash(ht, rht_key(ht, obj))]; *pp; pp = &(*pp)->next) {
		if (*pp == obj) {
			__atomic_store_n(pp, obj->next, __ATOMIC_RELEASE);
			atomic_dec(&ht->nelems);
			r = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&ht->lock, flags);
	return r;
}

int lkpi_rhl_insert(struct rhltable* hlt, struct rhlist_head* list)
{
	struct rhashtable* ht = &hlt->ht;
	const void* key = rht_key(ht, &list->rhead);
	u32 b = rht_hash(ht, key);
	unsigned long flags;
	struct rhash_head* he;

	spin_lock_irqsave(&ht->lock, flags);
	for (he = ht->buckets[b]; he; he = he->next) {
		if (rht_match(ht, he, key)) {
			/* Same key: chain behind the first entry. */
			struct rhlist_head* first = container_of(he, struct rhlist_head, rhead);
			list->next = first->next;
			list->rhead.next = NULL;
			__atomic_store_n(&first->next, list, __ATOMIC_RELEASE);
			atomic_inc(&ht->nelems);
			spin_unlock_irqrestore(&ht->lock, flags);
			return 0;
		}
	}
	list->next = NULL;
	list->rhead.next = ht->buckets[b];
	__atomic_store_n(&ht->buckets[b], &list->rhead, __ATOMIC_RELEASE);
	atomic_inc(&ht->nelems);
	spin_unlock_irqrestore(&ht->lock, flags);
	return 0;
}

int lkpi_rhl_remove(struct rhltable* hlt, struct rhlist_head* list)
{
	struct rhashtable* ht = &hlt->ht;
	unsigned long flags;
	struct rhash_head** pp;
	int r = -ENOENT;

	spin_lock_irqsave(&ht->lock, flags);
	for (pp = &ht->buckets[rht_hash(ht, rht_key(ht, &list->rhead))]; *pp; pp = &(*pp)->next) {
		struct rhlist_head* first = container_of(*pp, struct rhlist_head, rhead);
		struct rhlist_head** lp;
		if (first == list) {
			/* Head of a duplicate chain: promote the next duplicate. */
			if (list->next) {
				struct rhlist_head* n = list->next;
				n->rhead.next = list->rhead.next;
				__atomic_store_n(pp, &n->rhead, __ATOMIC_RELEASE);
			} else {
				__atomic_store_n(pp, list->rhead.next, __ATOMIC_RELEASE);
			}
			r = 0;
			break;
		}
		for (lp = &first->next; *lp; lp = &(*lp)->next) {
			if (*lp == list) {
				__atomic_store_n(lp, list->next, __ATOMIC_RELEASE);
				r = 0;
				goto out;
			}
		}
	}
out:
	if (!r)
		atomic_dec(&ht->nelems);
	spin_unlock_irqrestore(&ht->lock, flags);
	return r;
}

void rhashtable_walk_enter(struct rhashtable* ht, struct rhashtable_iter* iter)
{
	memset(iter, 0, sizeof(*iter));
	iter->ht = ht;
}

void rhashtable_walk_exit(struct rhashtable_iter* iter) {}

int rhashtable_walk_start_check(struct rhashtable_iter* iter)
{
	rcu_read_lock();
	return 0;
}

void* rhashtable_walk_next(struct rhashtable_iter* iter)
{
	struct rhashtable* ht = iter->ht;
	unsigned int skip = 0;
	/* Re-walk to position `skip` (tolerates concurrent changes). */
	for (unsigned int b = 0; b < LKPI_RHT_BUCKETS; b++) {
		for (struct rhash_head* he = READ_ONCE(ht->buckets[b]); he; he = READ_ONCE(he->next)) {
			if (ht->rhlist) {
				for (struct rhlist_head* l = container_of(he, struct rhlist_head, rhead); l; l = READ_ONCE(l->next)) {
					if (skip++ == iter->skip) {
						iter->skip++;
						return rht_obj(ht, &l->rhead);
					}
				}
			} else if (skip++ == iter->skip) {
				iter->skip++;
				return rht_obj(ht, he);
			}
		}
	}
	return NULL;
}

void rhashtable_walk_stop(struct rhashtable_iter* iter)
{
	rcu_read_unlock();
}

/* ---- hrtimer on timer_list ---- */

static void hrtimer_fire(struct timer_list* t)
{
	struct hrtimer* timer = container_of(t, struct hrtimer, lkpi_timer);
	enum hrtimer_restart r;
	timer->lkpi_active = 0;
	r = timer->function(timer);
	if (r == HRTIMER_RESTART)
		hrtimer_start(timer, timer->expires, HRTIMER_MODE_ABS);
}

void hrtimer_init(struct hrtimer* timer, int clock_id, enum hrtimer_mode mode)
{
	memset(timer, 0, sizeof(*timer));
	timer_setup(&timer->lkpi_timer, hrtimer_fire, 0);
}

void hrtimer_setup(struct hrtimer* timer, enum hrtimer_restart (*function)(struct hrtimer*), int clock_id,
		   enum hrtimer_mode mode)
{
	hrtimer_init(timer, clock_id, mode);
	timer->function = function;
}

void hrtimer_start(struct hrtimer* timer, ktime_t tim, const enum hrtimer_mode mode)
{
	ktime_t now = ktime_get();
	s64 delta_ns;
	timer->expires = (mode & HRTIMER_MODE_REL) ? now + tim : tim;
	delta_ns = timer->expires - now;
	timer->lkpi_active = 1;
	mod_timer(&timer->lkpi_timer, jiffies + (delta_ns > 0 ? (unsigned long)((delta_ns + 999999) / 1000000) : 0));
}

int hrtimer_cancel(struct hrtimer* timer)
{
	int was = timer->lkpi_active;
	del_timer_sync(&timer->lkpi_timer);
	timer->lkpi_active = 0;
	return was;
}

int hrtimer_try_to_cancel(struct hrtimer* timer)
{
	int was = timer->lkpi_active;
	del_timer(&timer->lkpi_timer);
	timer->lkpi_active = 0;
	return was;
}

u64 hrtimer_forward(struct hrtimer* timer, ktime_t now, ktime_t interval)
{
	u64 overruns = 0;
	if (interval <= 0)
		return 0;
	while (timer->expires <= now) {
		timer->expires += interval;
		overruns++;
	}
	return overruns;
}

/* ---- device core (no sysfs) ---- */

void device_initialize(struct device* dev)
{
	kref_init(&dev->kobj.kref);
}

int dev_set_name(struct device* dev, const char* fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	vsnprintf(dev->lkpi_name, sizeof(dev->lkpi_name), fmt, a);
	va_end(a);
	dev->init_name = NULL;
	dev->kobj.name = dev->lkpi_name;
	return 0;
}

int device_rename(struct device* dev, const char* new_name)
{
	strscpy(dev->lkpi_name, new_name, sizeof(dev->lkpi_name));
	dev->init_name = NULL;
	return 0;
}

int device_add(struct device* dev)
{
	if (dev->init_name) {
		strscpy(dev->lkpi_name, dev->init_name, sizeof(dev->lkpi_name));
		dev->init_name = NULL;
	}
	if (!dev->lkpi_registry && dev->parent)
		dev->lkpi_registry = dev->parent->lkpi_registry;
	dev->lkpi_added = true;
	return 0;
}

void device_del(struct device* dev)
{
	dev->lkpi_added = false;
}

void put_device(struct device* dev)
{
	if (!dev)
		return;
	if (kref_read(&dev->kobj.kref) > 0 && !refcount_dec_and_test(&dev->kobj.kref.refcount))
		return;
	if (dev->release)
		dev->release(dev);
	else if (dev->type && dev->type->release)
		dev->type->release(dev);
	else if (dev->class && dev->class->dev_release)
		dev->class->dev_release(dev);
}

int device_register(struct device* dev)
{
	device_initialize(dev);
	return device_add(dev);
}

void device_unregister(struct device* dev)
{
	device_del(dev);
	put_device(dev);
}

struct device* device_create(const struct class* cls, struct device* parent, u32 devt, void* drvdata, const char* fmt, ...)
{
	struct device* dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	va_list a;
	if (!dev)
		return ERR_PTR(-ENOMEM);
	device_initialize(dev);
	dev->class = cls;
	dev->parent = parent;
	dev->driver_data = drvdata;
	dev->release = (void (*)(struct device*))kfree;
	va_start(a, fmt);
	vsnprintf(dev->lkpi_name, sizeof(dev->lkpi_name), fmt, a);
	va_end(a);
	device_add(dev);
	return dev;
}

struct class* class_create(const char* name)
{
	struct class* cls = kzalloc(sizeof(*cls), GFP_KERNEL);
	if (!cls)
		return ERR_PTR(-ENOMEM);
	cls->name = name;
	return cls;
}

void class_destroy(const struct class* cls)
{
	if (!IS_ERR_OR_NULL(cls))
		kfree(cls);
}

/* ---- platform devices ---- */

static void platform_device_release(struct device* dev)
{
	kfree(to_platform_device(dev));
}

struct platform_device* platform_device_alloc(const char* name, int id)
{
	struct platform_device* pdev = kzalloc(sizeof(*pdev), GFP_KERNEL);
	if (!pdev)
		return NULL;
	pdev->name = name;
	pdev->id = id;
	device_initialize(&pdev->dev);
	pdev->dev.release = platform_device_release;
	if (id >= 0)
		dev_set_name(&pdev->dev, "%s.%d", name, id);
	else
		dev_set_name(&pdev->dev, "%s", name);
	return pdev;
}

int platform_device_add(struct platform_device* pdev)
{
	return device_add(&pdev->dev);
}

void platform_device_put(struct platform_device* pdev)
{
	if (pdev)
		put_device(&pdev->dev);
}

struct platform_device* platform_device_register_simple(const char* name, int id, const void* res, unsigned int num)
{
	struct platform_device* pdev = platform_device_alloc(name, id);
	if (!pdev)
		return ERR_PTR(-ENOMEM);
	platform_device_add(pdev);
	return pdev;
}

void platform_device_unregister(struct platform_device* pdev)
{
	if (!pdev)
		return;
	device_del(&pdev->dev);
	put_device(&pdev->dev);
}

int platform_driver_register(struct platform_driver* drv) { return 0; }
void platform_driver_unregister(struct platform_driver* drv) {}

/* ---- kthread workers ---- */
struct kthread_worker *kthread_create_worker(unsigned int flags, const char namefmt[], ...)
{
	struct kthread_worker *w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return ERR_PTR(-ENOMEM);
	w->wq = alloc_ordered_workqueue("%s", 0, namefmt);
	if (!w->wq) {
		kfree(w);
		return ERR_PTR(-ENOMEM);
	}
	return w;
}
void kthread_destroy_worker(struct kthread_worker *w)
{
	if (!w)
		return;
	destroy_workqueue(w->wq);
	kfree(w);
}

/* ---- xarray ---- */
#include <linux/xarray.h>
static int xa_grow(struct xarray *xa, unsigned long index)
{
	unsigned long n = xa->size ? xa->size : 16;
	void **s;
	while (n <= index)
		n *= 2;
	s = krealloc(xa->slots, n * sizeof(void *), GFP_ATOMIC);
	if (!s)
		return -ENOMEM;
	memset(s + xa->size, 0, (n - xa->size) * sizeof(void *));
	xa->slots = s;
	xa->size = n;
	return 0;
}
void *xa_load(struct xarray *xa, unsigned long index)
{
	void *e;
	spin_lock(&xa->xa_lock);
	e = index < xa->size ? xa->slots[index] : NULL;
	spin_unlock(&xa->xa_lock);
	return e;
}
void *xa_store(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp)
{
	void *old = NULL;
	spin_lock(&xa->xa_lock);
	if (index < xa->size || (entry && xa_grow(xa, index) == 0)) {
		if (index < xa->size) {
			old = xa->slots[index];
			xa->slots[index] = entry;
		}
	}
	spin_unlock(&xa->xa_lock);
	return old;
}
void *xa_erase(struct xarray *xa, unsigned long index) { return xa_store(xa, index, NULL, 0); }
int xa_alloc(struct xarray *xa, u32 *id, void *entry, struct xa_limit limit, gfp_t gfp)
{
	unsigned long i;
	int r = -EBUSY;
	spin_lock(&xa->xa_lock);
	for (i = limit.min; i <= limit.max; i++) {
		if (i >= xa->size && xa_grow(xa, i)) {
			r = -ENOMEM;
			break;
		}
		if (!xa->slots[i]) {
			xa->slots[i] = entry ? entry : (void *)1; /* reserved */
			*id = (u32)i;
			r = 0;
			break;
		}
	}
	spin_unlock(&xa->xa_lock);
	return r;
}
bool xa_empty(const struct xarray *xa)
{
	for (unsigned long i = 0; i < xa->size; i++)
		if (xa->slots[i])
			return false;
	return true;
}
void xa_destroy(struct xarray *xa)
{
	kfree(xa->slots);
	xa->slots = NULL;
	xa->size = 0;
}

/* ---- batch 2 helpers ---- */
#include <linux/scatterlist.h>
long simple_strtol(const char *cp, char **endp, unsigned int base)
{
	long sign = 1;
	if (*cp == '-') {
		sign = -1;
		cp++;
	}
	return sign * (long)simple_strtoul(cp, endp, base);
}
char *kvasprintf(gfp_t gfp, const char *fmt, va_list ap)
{
	va_list aq;
	int len;
	char *p;
	va_copy(aq, ap);
	len = vsnprintf(NULL, 0, fmt, aq);
	va_end(aq);
	p = kmalloc(len + 1, gfp);
	if (p)
		vsnprintf(p, len + 1, fmt, ap);
	return p;
}
int sg_alloc_table(struct sg_table *t, unsigned int nents, gfp_t gfp)
{
	t->sgl = kcalloc(nents ? nents : 1, sizeof(struct scatterlist), gfp);
	if (!t->sgl)
		return -ENOMEM;
	sg_init_table(t->sgl, nents ? nents : 1);
	t->nents = t->orig_nents = nents;
	return 0;
}
void sg_free_table(struct sg_table *t)
{
	kfree(t->sgl);
	t->sgl = NULL;
	t->nents = t->orig_nents = 0;
}

/* One pid object per call (DRM keeps it per client); only ->nr is read. */
struct pid *lkpi_task_pid(struct task_struct *t)
{
	struct pid *p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (p)
		p->nr = t ? t->tgid : 0;
	return p;
}

/* ---- batch 3 helpers ---- */
struct cpuinfo_x86 boot_cpu_data = { .x86_clflush_size = 64, .x86_cache_alignment = 64 };
struct resource iomem_resource = { .name = "PCI mem", .start = 0, .end = ~0ULL };

static void lkpi_swap(void *a, void *b, size_t size)
{
	char t, *x = a, *y = b;
	while (size--) {
		t = *x;
		*x++ = *y;
		*y++ = t;
	}
}
/* Heap sort (in place, O(n log n), like Linux's). */
void sort(void *base, size_t num, size_t size, int (*cmp)(const void *, const void *), void (*swap_fn)(void *, void *, int))
{
	char *b = base;
	size_t i, n, c, r;
	if (num < 2)
		return;
	for (i = num / 2; i-- > 0;) {
		for (r = i; (c = 2 * r + 1) < num; r = c) {
			if (c + 1 < num && cmp(b + c * size, b + (c + 1) * size) < 0)
				c++;
			if (cmp(b + r * size, b + c * size) >= 0)
				break;
			swap_fn ? swap_fn(b + r * size, b + c * size, size) : lkpi_swap(b + r * size, b + c * size, size);
		}
	}
	for (n = num - 1; n > 0; n--) {
		swap_fn ? swap_fn(b, b + n * size, size) : lkpi_swap(b, b + n * size, size);
		for (r = 0; (c = 2 * r + 1) < n; r = c) {
			if (c + 1 < n && cmp(b + c * size, b + (c + 1) * size) < 0)
				c++;
			if (cmp(b + r * size, b + c * size) >= 0)
				break;
			swap_fn ? swap_fn(b + r * size, b + c * size, size) : lkpi_swap(b + r * size, b + c * size, size);
		}
	}
}
/* Stable insertion sort of a list (DRM sorts short mode lists). */
void list_sort(void *priv, struct list_head *head, int (*cmp)(void *, const struct list_head *, const struct list_head *))
{
	struct list_head sorted, *e, *pos;
	INIT_LIST_HEAD(&sorted);
	while (!list_empty(head)) {
		e = head->next;
		list_del(e);
		for (pos = sorted.prev; pos != &sorted && cmp(priv, pos, e) > 0; pos = pos->prev)
			;
		list_add(e, pos);
	}
	list_splice(&sorted, head);
}
unsigned long clear_user(void __user *to, unsigned long n)
{
	char zero[64] = { 0 };
	while (n) {
		unsigned long c = n < sizeof(zero) ? n : sizeof(zero);
		if (copy_to_user(to, zero, c))
			return n;
		to = (char __user *)to + c;
		n -= c;
	}
	return 0;
}
void *devm_memremap(struct device *dev, resource_size_t offset, size_t size, unsigned long flags)
{
	return (void *)ioremap(offset, size);
}
void __iomem *devm_ioremap_wc(struct device *dev, resource_size_t offset, resource_size_t size)
{
	return ioremap(offset, size);
}

void *memdup_array_user(const void __user *src, size_t n, size_t size)
{
	void *p = kmalloc(n * size, GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);
	if (copy_from_user(p, src, n * size)) {
		kfree(p);
		return ERR_PTR(-EFAULT);
	}
	return p;
}

/* ---- I2C ---- */
#include <linux/i2c.h>
int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	int r = -EOPNOTSUPP, tries;
	int (*x)(struct i2c_adapter *, struct i2c_msg *, int) = adap->algo->master_xfer ? adap->algo->master_xfer : adap->algo->xfer;
	if (!x)
		return -EOPNOTSUPP;
	for (tries = 0; tries <= adap->retries; tries++) {
		r = x(adap, msgs, num);
		if (r != -EAGAIN)
			break;
	}
	return r;
}
int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	int r;
	mutex_lock(&adap->bus_lock);
	r = __i2c_transfer(adap, msgs, num);
	mutex_unlock(&adap->bus_lock);
	return r;
}
int i2c_add_adapter(struct i2c_adapter *adap)
{
	mutex_init(&adap->bus_lock);
	if (!adap->retries)
		adap->retries = 1;
	return 0;
}
void i2c_del_adapter(struct i2c_adapter *adap) { }
