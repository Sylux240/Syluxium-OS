/* LinuxKPI: Linux kernel programming interface for drivers ported to Syluxium.
 * Basic types. These headers are C (gnu11) and C++ compatible. */
#ifndef _LKPI_LINUX_TYPES_H
#define _LKPI_LINUX_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#ifdef __cplusplus
typedef uint64_t u64;   /* matches Syluxium's own u64 in C++ code */
#else
typedef unsigned long long u64; /* as in Linux, so printk's %llu is right */
#endif
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
#ifdef __cplusplus
typedef int64_t s64;
#else
typedef long long s64;
#endif
typedef u8 __u8;
typedef u16 __u16;
typedef u32 __u32;
typedef u64 __u64;
typedef s8 __s8;
typedef s16 __s16;
typedef s32 __s32;
typedef s64 __s64;

/* Endian-annotated types (sparse annotations in Linux; plain integers here). */
typedef u16 __le16;
typedef u32 __le32;
typedef u64 __le64;
typedef u16 __be16;
typedef u32 __be32;
typedef u64 __be64;
typedef u16 __sum16;
typedef u32 __wsum;

typedef long ssize_t;
typedef long long loff_t;
typedef u64 dma_addr_t;
typedef u64 phys_addr_t;
typedef u64 resource_size_t;
typedef unsigned int gfp_t;
typedef unsigned long kernel_ulong_t;
typedef unsigned int fmode_t;
typedef unsigned short umode_t;
typedef unsigned int __poll_t;
typedef unsigned int dev_t;

typedef struct {
    volatile int counter;
} atomic_t;
typedef struct {
    volatile long counter;
} atomic64_t;
typedef atomic64_t atomic_long_t;

struct list_head {
    struct list_head *next, *prev;
};
struct hlist_head {
    struct hlist_node* first;
};
struct hlist_node {
    struct hlist_node *next, **pprev;
};

#endif
