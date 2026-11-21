/* LinuxKPI: sockets of address families implemented by Linux code
 * (AF_BLUETOOTH, ...). A family registers with sock_register(); the
 * Syluxium socket layer creates struct sockets for it and calls its
 * proto_ops (kernel/linuxkpi/socket.cpp). */
#ifndef _LKPI_LINUX_NET_H
#define _LKPI_LINUX_NET_H
#include <linux/lkpi_all.h>
#include <linux/wait.h>
#include <linux/socket.h>
#include <linux/sockptr.h>
#include <linux/uio.h>

LKPI_EXTERN_C_BEGIN

typedef enum {
    SS_FREE = 0,
    SS_UNCONNECTED,
    SS_CONNECTING,
    SS_CONNECTED,
    SS_DISCONNECTING,
} socket_state;

enum sock_type {
    SOCK_STREAM = 1,
    SOCK_DGRAM = 2,
    SOCK_RAW = 3,
    SOCK_RDM = 4,
    SOCK_SEQPACKET = 5,
    SOCK_DCCP = 6,
    SOCK_PACKET = 10,
};
#define SOCK_MAX (SOCK_PACKET + 1)
#define SOCK_TYPE_MASK 0xf
#define SOCK_CLOEXEC 02000000
#define SOCK_NONBLOCK 00004000

#define SOCKWQ_ASYNC_NOSPACE 0
#define SOCKWQ_ASYNC_WAITDATA 1
#define SOCK_NOSPACE 2
#define SOCK_PASSCRED 3
#define SOCK_PASSSEC 4
#define SOCK_SUPPORT_ZC 5
#define SOCK_CUSTOM_SOCKOPT 6
#define SOCK_PASSPIDFD 7

struct socket_wq {
    wait_queue_head_t wait;
    struct fasync_struct *fasync_list;
    unsigned long flags;
    struct rcu_head rcu;
};

struct sock;
struct file;
struct poll_table_struct;
struct vm_area_struct;
struct seq_file;
struct pipe_inode_info;
struct net;

struct socket {
    socket_state state;
    short type;
    unsigned long flags;
    struct file *file;
    struct sock *sk;
    const struct proto_ops *ops;
    struct socket_wq wq;
    void *lkpi_native; /* the Syluxium socket (net::Socket) using it, if any */
};

struct proto_accept_arg {
    int flags;
    int err;
    int is_empty;
    bool kern;
};

typedef struct {
    size_t written;
    size_t count;
    union {
        char __user *buf;
        void *data;
    } arg;
    int error;
} read_descriptor_t;
struct sk_buff;
typedef int (*sk_read_actor_t)(read_descriptor_t *, struct sk_buff *, unsigned int, size_t);
typedef int (*skb_read_actor_t)(struct sock *, struct sk_buff *);

struct proto_ops {
    int family;
    struct module *owner;
    int (*release)(struct socket *sock);
    int (*bind)(struct socket *sock, struct sockaddr *myaddr, int sockaddr_len);
    int (*connect)(struct socket *sock, struct sockaddr *vaddr, int sockaddr_len, int flags);
    int (*socketpair)(struct socket *sock1, struct socket *sock2);
    int (*accept)(struct socket *sock, struct socket *newsock, struct proto_accept_arg *arg);
    int (*getname)(struct socket *sock, struct sockaddr *addr, int peer);
    __poll_t (*poll)(struct file *file, struct socket *sock, struct poll_table_struct *wait);
    int (*ioctl)(struct socket *sock, unsigned int cmd, unsigned long arg);
    int (*compat_ioctl)(struct socket *sock, unsigned int cmd, unsigned long arg);
    int (*gettstamp)(struct socket *sock, void __user *userstamp, bool timeval, bool time32);
    int (*listen)(struct socket *sock, int len);
    int (*shutdown)(struct socket *sock, int flags);
    int (*setsockopt)(struct socket *sock, int level, int optname, sockptr_t optval, unsigned int optlen);
    int (*getsockopt)(struct socket *sock, int level, int optname, char __user *optval, int __user *optlen);
    void (*show_fdinfo)(struct seq_file *m, struct socket *sock);
    int (*sendmsg)(struct socket *sock, struct msghdr *m, size_t total_len);
    int (*recvmsg)(struct socket *sock, struct msghdr *m, size_t total_len, int flags);
    int (*mmap)(struct file *file, struct socket *sock, struct vm_area_struct *vma);
    ssize_t (*splice_read)(struct socket *sock, loff_t *ppos, struct pipe_inode_info *pipe, size_t len,
                           unsigned int flags);
    void (*splice_eof)(struct socket *sock);
    int (*set_peek_off)(struct sock *sk, int val);
    int (*peek_len)(struct socket *sock);
    int (*read_sock)(struct sock *sk, read_descriptor_t *desc, sk_read_actor_t recv_actor);
    int (*read_skb)(struct sock *sk, skb_read_actor_t recv_actor);
    int (*sendmsg_locked)(struct sock *sk, struct msghdr *msg, size_t size);
    int (*set_rcvlowat)(struct sock *sk, int val);
};

struct net_proto_family {
    int family;
    int (*create)(struct net *net, struct socket *sock, int protocol, int kern);
    struct module *owner;
};

int sock_register(const struct net_proto_family *fam);
void sock_unregister(int family);
const struct net_proto_family *lkpi_sock_family(int family);
int sock_create_kern(struct net *net, int family, int type, int proto, struct socket **res);
struct socket *sock_alloc(void);
void sock_release(struct socket *sock);
int kernel_sendmsg(struct socket *sock, struct msghdr *msg, struct kvec *vec, size_t num, size_t len);
int kernel_recvmsg(struct socket *sock, struct msghdr *msg, struct kvec *vec, size_t num, size_t len, int flags);
int kernel_bind(struct socket *sock, struct sockaddr *addr, int addrlen);
int kernel_listen(struct socket *sock, int backlog);
int kernel_accept(struct socket *sock, struct socket **newsock, int flags);
int kernel_connect(struct socket *sock, struct sockaddr *addr, int addrlen, int flags);
int kernel_getsockname(struct socket *sock, struct sockaddr *addr);
int kernel_getpeername(struct socket *sock, struct sockaddr *addr);
int kernel_sock_shutdown(struct socket *sock, int how);
int sock_sendmsg(struct socket *sock, struct msghdr *msg);
int sock_recvmsg(struct socket *sock, struct msghdr *msg, int flags);

/* proto_ops fillers for what a family doesn't do. */
int sock_no_bind(struct socket *, struct sockaddr *, int);
int sock_no_connect(struct socket *, struct sockaddr *, int, int);
int sock_no_socketpair(struct socket *, struct socket *);
int sock_no_accept(struct socket *, struct socket *, struct proto_accept_arg *);
int sock_no_getname(struct socket *, struct sockaddr *, int);
int sock_no_ioctl(struct socket *, unsigned int, unsigned long);
int sock_no_listen(struct socket *, int);
int sock_no_shutdown(struct socket *, int);
int sock_no_sendmsg(struct socket *, struct msghdr *, size_t);
int sock_no_sendmsg_locked(struct sock *sk, struct msghdr *msg, size_t len);
int sock_no_recvmsg(struct socket *, struct msghdr *, size_t, int);
int sock_no_mmap(struct file *file, struct socket *sock, struct vm_area_struct *vma);
int sock_common_setsockopt(struct socket *sock, int level, int optname, sockptr_t optval, unsigned int optlen);
int sock_common_getsockopt(struct socket *sock, int level, int optname, char __user *optval, int __user *optlen);

/* Called by Linux code when a socket's state changes: wakes the Syluxium
 * socket's pollers. */
void lkpi_sock_notify(struct socket *sock);

#define MODULE_ALIAS_NETPROTO(proto)
#define net_ratelimit() 1
#define sockfd_lookup(fd, err) ((struct socket *)(*(err) = -ENOTSOCK, NULL))
#define sockfd_put(sock) do { } while (0)

LKPI_EXTERN_C_END
#endif
