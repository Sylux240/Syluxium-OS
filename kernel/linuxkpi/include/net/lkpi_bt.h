/* LinuxKPI: what Linux's Bluetooth core (net/bluetooth) and drivers need
 * beyond the common networking set. Included first (Makefile). */
#pragma once

#include <net/lkpi_netcompat.h>
#include <linux/net.h>
#include <net/sock.h>
#include <linux/capability.h>

/* On-stack structures with a flexible array of constant size. */
#ifndef DEFINE_FLEX
#define _DEFINE_FLEX(type, name, member, count, initializer...)                                    \
    union {                                                                                        \
        u8 bytes[struct_size_t(type, member, count)];                                              \
        type obj;                                                                                  \
    } name##_u initializer;                                                                        \
    type *name = (type *)&name##_u
#define DEFINE_FLEX(TYPE, NAME, MEMBER, COUNTER, COUNT) \
    _DEFINE_FLEX(TYPE, NAME, MEMBER, COUNT, = { .obj.COUNTER = COUNT, })
#define DEFINE_RAW_FLEX(type, name, member, count) _DEFINE_FLEX(type, name, member, count, = {})
#endif

/* skbs: no time stamps, no reserved tailroom; copies are full copies. */
static inline void skb_tailroom_reserve(struct sk_buff *skb, unsigned int mtu, unsigned int needed_tailroom) {}
static inline void __net_timestamp(struct sk_buff *skb) {}
static inline ktime_t skb_get_ktime(const struct sk_buff *skb) { return ktime_get_real(); }
static inline struct sk_buff *__pskb_copy_fclone(struct sk_buff *skb, int headroom, gfp_t gfp, bool fclone)
{
    struct sk_buff *n = skb_copy(skb, gfp);
    if (n && skb_headroom(n) < headroom && pskb_expand_head(n, headroom - skb_headroom(n), 0, gfp)) {
        kfree_skb(n);
        return NULL;
    }
    return n;
}
#ifndef __pskb_copy
#define __pskb_copy(skb, headroom, gfp) __pskb_copy_fclone(skb, headroom, gfp, false)
#endif

#ifndef __RW_LOCK_UNLOCKED
#define __RW_LOCK_UNLOCKED(name) __SPIN_LOCK_UNLOCKED(name)
#endif
#ifndef SINGLE_DEPTH_NESTING
#define SINGLE_DEPTH_NESTING 1
#endif
#define security_sk_clone(sk, newsk) do { } while (0)
static inline int sock_gettstamp(struct socket *sock, void __user *userstamp, bool timeval, bool time32)
{
    return -ENOENT;
}
#ifndef DPM_ORDER_DEV_LAST
#define DPM_ORDER_DEV_LAST 3
#endif
static inline int device_move(struct device *dev, struct device *new_parent, int dpm_order)
{
    dev->parent = new_parent;
    return 0;
}
#define __module_get(m) do { } while (0)
#include <net/scm.h>
#include <crypto/hash.h>
#include <linux/property.h>

#define DECLARE_SOCKADDR(type, dst, src) type dst = (type)(src)
#ifndef get_current_cred
#define get_current_cred() lkpi_current_cred()
#endif
#ifndef MSG_CMSG_COMPAT
#define MSG_CMSG_COMPAT 0
#endif
#define trace_sk_data_ready(sk) do { } while (0)
struct __kernel_old_timeval {
    long tv_sec;
    long tv_usec;
};
static inline void skb_get_timestamp(const struct sk_buff *skb, struct __kernel_old_timeval *stamp)
{
    stamp->tv_sec = 0;
    stamp->tv_usec = 0;
}
#ifndef disable_delayed_work
#define disable_delayed_work(d) cancel_delayed_work(d)
#endif
static inline int fwnode_property_read_u8_array(const struct fwnode_handle *fwnode, const char *propname, u8 *val,
                                                size_t nval)
{
    return -EINVAL;
}
static inline struct device *device_find_child(struct device *parent, void *data,
                                               int (*match)(struct device *dev, void *data))
{
    return NULL;
}
static inline const char *kvasprintf_const(gfp_t gfp, const char *fmt, va_list ap) { return kvasprintf(gfp, fmt, ap); }
static inline int vsprintf(char *buf, const char *fmt, va_list args) { return vsnprintf(buf, 1024, fmt, args); }
/* One-shot keyed hash (cmac(aes) for Bluetooth pairing). */
static inline int crypto_shash_tfm_digest(struct crypto_shash *tfm, const u8 *data, unsigned int len, u8 *out)
{
    SHASH_DESC_ON_STACK(desc, tfm);
    desc->tfm = tfm;
    return crypto_shash_digest(desc, data, len, out);
}
static inline void memcpy_and_pad(void *dest, size_t dest_len, const void *src, size_t count, int pad)
{
    if (dest_len > count) {
        __builtin_memcpy(dest, src, count);
        __builtin_memset((char *)dest + count, pad, dest_len - count);
    } else {
        __builtin_memcpy(dest, src, dest_len);
    }
}
#include <linux/pm_runtime.h>
