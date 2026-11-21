/* LinuxKPI: I/O vectors. The socket bridge hands Linux code kernel buffers
 * (data already copied in from the program), so only kernel vectors
 * (ITER_KVEC) exist. */
#ifndef _LKPI_LINUX_UIO_H
#define _LKPI_LINUX_UIO_H
#include <linux/lkpi_all.h>

LKPI_EXTERN_C_BEGIN

struct iovec {
    void __user *iov_base;
    __kernel_size_t iov_len;
};
struct kvec {
    void *iov_base;
    size_t iov_len;
};

enum iter_type { ITER_UBUF, ITER_IOVEC, ITER_BVEC, ITER_KVEC, ITER_FOLIOQ, ITER_XARRAY, ITER_DISCARD };
#define ITER_SOURCE 1 /* == WRITE */
#define ITER_DEST 0   /* == READ */

struct iov_iter {
    u8 iter_type;
    bool nofault;
    bool data_source;
    size_t iov_offset; /* within the current segment */
    size_t count;      /* bytes left */
    const struct kvec *kvec;
    unsigned long nr_segs;
};

static inline void iov_iter_kvec(struct iov_iter *i, unsigned int direction, const struct kvec *kvec,
                                 unsigned long nr_segs, size_t count)
{
    i->iter_type = ITER_KVEC;
    i->nofault = false;
    i->data_source = direction;
    i->iov_offset = 0;
    i->count = count;
    i->kvec = kvec;
    i->nr_segs = nr_segs;
}
static inline size_t iov_iter_count(const struct iov_iter *i) { return i->count; }
static inline bool iov_iter_is_kvec(const struct iov_iter *i) { return i->iter_type == ITER_KVEC; }
static inline bool user_backed_iter(const struct iov_iter *i) { return false; }

/* Walk `bytes` of the iterator, calling step(segment pointer, length). */
#define lkpi_iov_walk(i, bytes, step)                                                              \
    ({                                                                                             \
        size_t __left = (bytes) < (i)->count ? (bytes) : (i)->count, __done = 0;                   \
        while (__left && (i)->nr_segs) {                                                           \
            size_t __avail = (i)->kvec->iov_len - (i)->iov_offset;                                 \
            size_t __n = __avail < __left ? __avail : __left;                                      \
            if (__n) {                                                                             \
                char *__p = (char *)(i)->kvec->iov_base + (i)->iov_offset;                         \
                step(__p, __n, __done);                                                            \
            }                                                                                      \
            (i)->iov_offset += __n;                                                                \
            (i)->count -= __n;                                                                     \
            __left -= __n;                                                                         \
            __done += __n;                                                                         \
            if ((i)->iov_offset == (i)->kvec->iov_len) {                                           \
                (i)->kvec++;                                                                       \
                (i)->nr_segs--;                                                                    \
                (i)->iov_offset = 0;                                                               \
            }                                                                                      \
        }                                                                                          \
        __done;                                                                                    \
    })

static inline size_t copy_from_iter(void *addr, size_t bytes, struct iov_iter *i)
{
#define __lkpi_cf(p, n, done) __builtin_memcpy((char *)addr + (done), (p), (n))
    return lkpi_iov_walk(i, bytes, __lkpi_cf);
#undef __lkpi_cf
}
static inline size_t copy_to_iter(const void *addr, size_t bytes, struct iov_iter *i)
{
#define __lkpi_ct(p, n, done) __builtin_memcpy((p), (const char *)addr + (done), (n))
    return lkpi_iov_walk(i, bytes, __lkpi_ct);
#undef __lkpi_ct
}
static inline bool copy_from_iter_full(void *addr, size_t bytes, struct iov_iter *i)
{
    if (bytes > i->count)
        return false;
    return copy_from_iter(addr, bytes, i) == bytes;
}
static inline void iov_iter_advance(struct iov_iter *i, size_t bytes)
{
#define __lkpi_skip(p, n, done) do { } while (0)
    lkpi_iov_walk(i, bytes, __lkpi_skip);
#undef __lkpi_skip
}
#define copy_from_iter_nocache copy_from_iter
#define _copy_from_iter copy_from_iter
#define _copy_to_iter copy_to_iter

LKPI_EXTERN_C_END
#endif
