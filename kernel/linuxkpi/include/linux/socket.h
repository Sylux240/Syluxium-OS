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

#define AF_BLUETOOTH 31
#define PF_BLUETOOTH AF_BLUETOOTH
#define AF_MAX 46
#define SOL_SOCKET 1
#define SOL_BLUETOOTH 274

#define MSG_OOB 1
#define MSG_PEEK 2
#define MSG_DONTROUTE 4
#define MSG_TRYHARD 4
#define MSG_CTRUNC 8
#define MSG_PROBE 0x10
#define MSG_TRUNC 0x20
#define MSG_EOR 0x80
#define MSG_WAITALL 0x100
#define MSG_FIN 0x200
#define MSG_SYN 0x400
#define MSG_CONFIRM 0x800
#define MSG_RST 0x1000
#define MSG_ERRQUEUE 0x2000
#define MSG_NOSIGNAL 0x4000
#define MSG_MORE 0x8000
#define MSG_WAITFORONE 0x10000
#define MSG_CMSG_CLOEXEC 0x40000000

#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

#include <linux/uio.h>

/* What sendmsg/recvmsg hand a socket family: kernel buffers, the address,
 * and room for control messages (none are passed through yet). */
struct kiocb;
struct msghdr {
    void *msg_name;
    int msg_namelen;
    int msg_inq;
    struct iov_iter msg_iter;
    union {
        void *msg_control;
        void __user *msg_control_user;
    };
    bool msg_control_is_user : 1;
    bool msg_get_inq : 1;
    unsigned int msg_flags;
    __kernel_size_t msg_controllen;
    struct kiocb *msg_iocb;
    void *msg_ubuf;
};

struct cmsghdr {
    __kernel_size_t cmsg_len;
    int cmsg_level;
    int cmsg_type;
};
#define SCM_RIGHTS 1
#define SCM_CREDENTIALS 2
#define SCM_TIMESTAMP 29

static inline size_t msg_data_left(struct msghdr *msg) { return iov_iter_count(&msg->msg_iter); }
static inline int memcpy_from_msg(void *data, struct msghdr *msg, int len)
{
    return copy_from_iter_full(data, len, &msg->msg_iter) ? 0 : -EFAULT;
}
static inline int memcpy_to_msg(struct msghdr *msg, void *data, int len)
{
    return copy_to_iter(data, len, &msg->msg_iter) == (size_t)len ? 0 : -EFAULT;
}
/* Control messages aren't passed to programs yet. */
static inline int put_cmsg(struct msghdr *msg, int level, int type, int len, void *data)
{
    msg->msg_flags |= MSG_CTRUNC;
    return 0;
}

#endif
