/* LinuxKPI: socket address definitions. */
#ifndef _LKPI_LINUX_SOCKET_H
#define _LKPI_LINUX_SOCKET_H

#include <linux/types.h>

typedef unsigned short sa_family_t;
typedef unsigned short __kernel_sa_family_t;
typedef char* __kernel_caddr_t;

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct __kernel_sockaddr_storage {
    union {
        struct {
            __kernel_sa_family_t ss_family;
            char __data[126];
        };
        void* __align;
    };
};
#define sockaddr_storage __kernel_sockaddr_storage

#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_INET 2
#define AF_INET6 10
#define AF_NETLINK 16
#define AF_PACKET 17
#define PF_UNSPEC AF_UNSPEC
#define PF_INET AF_INET
#define PF_INET6 AF_INET6
#define PF_NETLINK AF_NETLINK
#define PF_PACKET AF_PACKET
#define SOL_NETLINK 270
#define MSG_DONTWAIT 0x40

#endif
