#ifndef _LKPI_LINUX_UACCESS_H
#define _LKPI_LINUX_UACCESS_H
#include <linux/kernel.h>
LKPI_EXTERN_C_BEGIN
unsigned long copy_to_user(void __user* to, const void* from, unsigned long n);
unsigned long copy_from_user(void* to, const void __user* from, unsigned long n);
#define put_user(x, p) ({ __typeof__(*(p)) __v = (x); copy_to_user((p), &__v, sizeof(__v)) ? -EFAULT : 0; })
#define get_user(x, p) ({ __typeof__(*(p) + 0) __v; int __r = copy_from_user(&__v, (p), sizeof(__v)) ? -EFAULT : 0; (x) = (__typeof__(x))__v; __r; })
/* A NUL-terminated string of at most `count` bytes; its length, or -EFAULT. */
static inline long strncpy_from_user(char *dst, const char __user *src, long count)
{
    long i;
    for (i = 0; i < count; i++) {
        if (copy_from_user(&dst[i], src + i, 1))
            return -EFAULT;
        if (!dst[i])
            return i;
    }
    return count;
}
/* 1 if the `size` bytes at `from` are all zero, 0 if not, -EFAULT. */
static inline int check_zeroed_user(const void __user *from, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        unsigned char c;
        if (copy_from_user(&c, (const char __user *)from + i, 1))
            return -EFAULT;
        if (c)
            return 0;
    }
    return 1;
}
static inline int copy_struct_from_user(void *dst, size_t ksize, const void __user *src, size_t usize)
{
    size_t size = ksize < usize ? ksize : usize;
    if (usize > ksize) {
        int r = check_zeroed_user((const char __user *)src + ksize, usize - ksize);
        if (r <= 0)
            return r ? r : -E2BIG;
    } else if (usize < ksize) {
        __builtin_memset((char *)dst + size, 0, ksize - size);
    }
    return copy_from_user(dst, src, size) ? -EFAULT : 0;
}
LKPI_EXTERN_C_END
#endif
