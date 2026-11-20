#ifndef _LKPI_LINUX_UACCESS_H
#define _LKPI_LINUX_UACCESS_H
#include <linux/kernel.h>
LKPI_EXTERN_C_BEGIN
unsigned long copy_to_user(void __user* to, const void* from, unsigned long n);
unsigned long copy_from_user(void* to, const void __user* from, unsigned long n);
#define put_user(x, p) ({ __typeof__(*(p)) __v = (x); copy_to_user((p), &__v, sizeof(__v)) ? -EFAULT : 0; })
#define get_user(x, p) ({ __typeof__(*(p) + 0) __v; int __r = copy_from_user(&__v, (p), sizeof(__v)) ? -EFAULT : 0; (x) = (__typeof__(x))__v; __r; })
LKPI_EXTERN_C_END
#endif
