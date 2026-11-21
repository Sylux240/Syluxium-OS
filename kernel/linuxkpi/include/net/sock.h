/* LinuxKPI: struct sock, for address families implemented by Linux code
 * (AF_BLUETOOTH) and what the wireless stack touches on skb->sk. The
 * fields before the "appended" mark are the original ones and stay in
 * place. Implementation: glue/sock.c. */
#ifndef _LKPI_NET_SOCK_H
#define _LKPI_NET_SOCK_H
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/net.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <net/net_namespace.h>
#include <linux/lkpi_binder.h> /* kuid_t, creds */

LKPI_EXTERN_C_BEGIN

struct proto;
struct pid;
struct cred;

struct sock {
    possible_net_t sk_net;
    int sk_protocol;
    u8 sk_pacing_shift;
    u32 sk_txhash;
    u32 sk_mark;
    unsigned long sk_flags;
    refcount_t sk_refcnt;
    u32 sk_tskey;
    u16 sk_tsflags;
    /* appended: the socket layer */
    struct hlist_node sk_node;
    unsigned short sk_family;
    unsigned short sk_type;
    volatile unsigned char sk_state;
    unsigned char sk_reuse;
    unsigned char sk_shutdown;
    unsigned char sk_userlocks;
    int sk_bound_dev_if;
    struct proto *sk_prot;
    struct socket *sk_socket;
    struct sk_buff_head sk_receive_queue;
    struct sk_buff_head sk_write_queue;
    struct sk_buff_head sk_error_queue;
    int sk_err;
    int sk_err_soft;
    int sk_rcvbuf;
    int sk_sndbuf;
    atomic_t sk_rmem_alloc;
    refcount_t sk_wmem_alloc;
    long sk_rcvtimeo;
    long sk_sndtimeo;
    int sk_rcvlowat;
    u32 sk_priority;
    unsigned long sk_lingertime;
    u32 sk_ack_backlog;
    u32 sk_max_ack_backlog;
    void *sk_user_data;
    struct pid *sk_peer_pid;
    const struct cred *sk_peer_cred;
    spinlock_t sk_peer_lock;
    rwlock_t sk_callback_lock;
    void (*sk_state_change)(struct sock *sk);
    void (*sk_data_ready)(struct sock *sk);
    void (*sk_write_space)(struct sock *sk);
    void (*sk_error_report)(struct sock *sk);
    void (*sk_destruct)(struct sock *sk);
    struct socket_wq lkpi_wq;   /* what sk_sleep() returns */
    struct mutex lkpi_owner;    /* lock_sock(): held by a process-context user */
    spinlock_t lkpi_slock;      /* bh_lock_sock() */
    unsigned long lkpi_ino;     /* sock_i_ino() */
};

/* sk_state values a few families share (TCP's). */
enum {
    TCP_ESTABLISHED = 1,
    TCP_SYN_SENT,
    TCP_SYN_RECV,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_TIME_WAIT,
    TCP_CLOSE,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_LISTEN,
    TCP_CLOSING,
    TCP_NEW_SYN_RECV,
    TCP_BOUND_INACTIVE,
    TCP_MAX_STATES
};
#define RCV_SHUTDOWN 1
#define SEND_SHUTDOWN 2
#define SHUTDOWN_MASK 3
#define SOCK_SNDBUF_LOCK 1
#define SOCK_RCVBUF_LOCK 2
#define SK_RMEM_MAX (212992)
#define SK_WMEM_MAX (212992)

enum sock_flags {
    SOCK_DEAD,
    SOCK_DONE,
    SOCK_URGINLINE,
    SOCK_KEEPOPEN,
    SOCK_LINGER,
    SOCK_DESTROY,
    SOCK_BROADCAST,
    SOCK_TIMESTAMP,
    SOCK_ZAPPED,
    SOCK_USE_WRITE_QUEUE,
    SOCK_DBG,
    SOCK_RCVTSTAMP,
    SOCK_RCVTSTAMPNS,
    SOCK_LOCALROUTE,
    SOCK_MEMALLOC,
    SOCK_TIMESTAMPING_RX_SOFTWARE,
    SOCK_FASYNC,
    SOCK_RXQ_OVFL,
    SOCK_ZEROCOPY,
    SOCK_WIFI_STATUS,
    SOCK_NOFCS,
    SOCK_FILTER_LOCKED,
    SOCK_SELECT_ERR_QUEUE,
    SOCK_RCU_FREE,
    SOCK_TXTIME,
    SOCK_XDP,
    SOCK_TSTAMP_NEW,
    SOCK_RCVMARK,
};
#define SOF_TIMESTAMPING_OPT_ID (1 << 7)
#define SOF_TIMESTAMPING_TX_HARDWARE (1 << 0)

/* A protocol's socket size and hooks (sk_alloc allocates obj_size). */
struct proto {
    char name[32];
    struct module *owner;
    unsigned int obj_size;
    void (*close)(struct sock *sk, long timeout);
    void (*destroy)(struct sock *sk);
};
int proto_register(struct proto *prot, int alloc_slab);
void proto_unregister(struct proto *prot);

struct sock *sk_alloc(struct net *net, int family, gfp_t priority, struct proto *prot, int kern);
void sk_free(struct sock *sk);
void sock_init_data(struct socket *sock, struct sock *sk);
#define sock_init_data_uid(sock, sk, uid) sock_init_data(sock, sk)

static inline struct net *sock_net(const struct sock *sk) { return read_pnet(&sk->sk_net); }
static inline void sock_hold(struct sock *sk) { if (sk) refcount_inc(&sk->sk_refcnt); }
static inline void __sock_put(struct sock *sk) { refcount_dec(&sk->sk_refcnt); }
static inline void sock_put(struct sock *sk)
{
    if (sk && refcount_dec_and_test(&sk->sk_refcnt))
        sk_free(sk);
}
static inline void sock_efree(struct sk_buff *skb) {}
static inline int sk_fullsock(const struct sock *sk) { return 1; }
static inline void sk_pacing_shift_update(struct sock *sk, int val)
{
    if (sk && sk->sk_pacing_shift != val)
        sk->sk_pacing_shift = val;
}
static inline bool sock_flag(const struct sock *sk, enum sock_flags flag) { return test_bit(flag, &sk->sk_flags); }
static inline void sock_set_flag(struct sock *sk, enum sock_flags flag) { set_bit(flag, &sk->sk_flags); }
static inline void sock_reset_flag(struct sock *sk, enum sock_flags flag) { clear_bit(flag, &sk->sk_flags); }
static inline void sock_valbool_flag(struct sock *sk, enum sock_flags bit, int valbool)
{
    if (valbool)
        sock_set_flag(sk, bit);
    else
        sock_reset_flag(sk, bit);
}
static inline void sk_set_bit(int nr, struct sock *sk) { if (sk->sk_socket) set_bit(nr, &sk->sk_socket->flags); }
static inline void sk_clear_bit(int nr, struct sock *sk) { if (sk->sk_socket) clear_bit(nr, &sk->sk_socket->flags); }
static inline void skb_set_owner_w(struct sk_buff *skb, struct sock *sk) { skb->sk = sk; }
static inline void skb_set_owner_r(struct sk_buff *skb, struct sock *sk) { skb->sk = sk; }
static inline int sock_queue_err_skb(struct sock *sk, struct sk_buff *skb) { kfree_skb(skb); return 0; }

static inline wait_queue_head_t *sk_sleep(struct sock *sk) { return &sk->lkpi_wq.wait; }
static inline bool skwq_has_sleeper(struct socket_wq *wq) { return wq && waitqueue_active(&wq->wait); }
static inline void sock_graft(struct sock *sk, struct socket *parent)
{
    parent->sk = sk;
    sk->sk_socket = parent;
}
static inline void sock_orphan(struct sock *sk)
{
    sock_set_flag(sk, SOCK_DEAD);
    sk->sk_socket = NULL;
}

/* lock_sock: process context owns the socket (a sleeping lock). */
void lock_sock_nested(struct sock *sk, int subclass);
static inline void lock_sock(struct sock *sk) { lock_sock_nested(sk, 0); }
void release_sock(struct sock *sk);
#define sock_owned_by_user(sk) mutex_is_locked(&(sk)->lkpi_owner)
#define sock_owned_by_user_nocheck(sk) sock_owned_by_user(sk)
#define lockdep_sock_is_held(sk) 1
#define bh_lock_sock(sk) spin_lock(&(sk)->lkpi_slock)
#define bh_unlock_sock(sk) spin_unlock(&(sk)->lkpi_slock)
#define bh_lock_sock_nested(sk) bh_lock_sock(sk)
#define sock_lock_init_class_and_name(sk, sname, skey, name, key) do { } while (0)
#define sock_allow_reclassification(sk) true
#define sk_wait_event(sk, timeo, condition, wait)                                                  \
    ({                                                                                             \
        int __rc;                                                                                  \
        release_sock(sk);                                                                          \
        __rc = __condition_lkpi(condition);                                                        \
        if (!__rc)                                                                                 \
            *(timeo) = wait_woken(wait, TASK_INTERRUPTIBLE, *(timeo));                             \
        lock_sock(sk);                                                                             \
        __rc = __condition_lkpi(condition);                                                        \
        __rc;                                                                                      \
    })
#define __condition_lkpi(c) (c)

static inline long sock_rcvtimeo(const struct sock *sk, bool noblock) { return noblock ? 0 : sk->sk_rcvtimeo; }
static inline long sock_sndtimeo(const struct sock *sk, bool noblock) { return noblock ? 0 : sk->sk_sndtimeo; }
static inline int sock_rcvlowat(const struct sock *sk, int waitall, int len)
{
    int v = waitall ? len : (sk->sk_rcvlowat < len ? sk->sk_rcvlowat : len);
    return v ? v : 1;
}
static inline int sock_intr_errno(long timeo) { return timeo == MAX_SCHEDULE_TIMEOUT ? -ERESTARTSYS : -EINTR; }
static inline int sock_error(struct sock *sk)
{
    int err = __atomic_exchange_n(&sk->sk_err, 0, __ATOMIC_SEQ_CST);
    return -err;
}
static inline bool sock_writeable(const struct sock *sk) { return true; }
static inline int sk_rmem_alloc_get(const struct sock *sk) { return atomic_read(&sk->sk_rmem_alloc); }
static inline int sk_wmem_alloc_get(const struct sock *sk) { return 0; }
static inline bool sk_acceptq_is_full(const struct sock *sk) { return sk->sk_ack_backlog > sk->sk_max_ack_backlog; }
static inline void sk_acceptq_added(struct sock *sk) { sk->sk_ack_backlog++; }
static inline void sk_acceptq_removed(struct sock *sk) { sk->sk_ack_backlog--; }
static inline int sk_filter(struct sock *sk, struct sk_buff *skb) { return 0; }
static inline bool sk_capable(const struct sock *sk, int cap) { return capable(cap); }
static inline unsigned long sock_i_ino(struct sock *sk) { return sk->lkpi_ino; }
kuid_t sock_i_uid(struct sock *sk);
#define sock_skb_cb_check_size(size) BUILD_BUG_ON((size) > sizeof_field(struct sk_buff, cb))
static inline void sock_recv_timestamp(struct msghdr *msg, struct sock *sk, struct sk_buff *skb) {}
static inline void sock_recv_cmsgs(struct msghdr *msg, struct sock *sk, struct sk_buff *skb) {}
#define sk_error_report(sk) ((sk)->sk_error_report(sk))

/* Socket lists (hlists of sk_node). */
#define sk_entry(node) hlist_entry(node, struct sock, sk_node)
#define sk_for_each(__sk, list) hlist_for_each_entry(__sk, list, sk_node)
#define sk_for_each_safe(__sk, tmp, list) hlist_for_each_entry_safe(__sk, tmp, list, sk_node)
static inline void sk_add_node(struct sock *sk, struct hlist_head *list)
{
    sock_hold(sk);
    hlist_add_head(&sk->sk_node, list);
}
static inline bool sk_del_node_init(struct sock *sk)
{
    if (hlist_unhashed(&sk->sk_node))
        return false;
    hlist_del_init(&sk->sk_node);
    __sock_put(sk);
    return true;
}
static inline bool sk_unhashed(const struct sock *sk) { return hlist_unhashed(&sk->sk_node); }

/* Receive queue and datagrams (glue/sock.c). */
int sock_queue_rcv_skb(struct sock *sk, struct sk_buff *skb);
#define __sock_queue_rcv_skb sock_queue_rcv_skb
struct sk_buff *sock_alloc_send_skb(struct sock *sk, unsigned long size, int noblock, int *errcode);
struct sk_buff *sock_alloc_send_pskb(struct sock *sk, unsigned long header_len, unsigned long data_len,
                                     int noblock, int *errcode, int max_page_order);
struct sk_buff *skb_recv_datagram(struct sock *sk, unsigned int flags, int *err);
void skb_free_datagram(struct sock *sk, struct sk_buff *skb);
int skb_copy_datagram_msg(const struct sk_buff *from, int offset, struct msghdr *msg, int size);
#define skb_copy_datagram_iter(from, offset, iter, size) lkpi_skb_copy_to_iter(from, offset, iter, size)
int lkpi_skb_copy_to_iter(const struct sk_buff *from, int offset, struct iov_iter *to, int size);
__poll_t datagram_poll(struct file *file, struct socket *sock, struct poll_table_struct *wait);
void sock_def_readable(struct sock *sk);

LKPI_EXTERN_C_END
#endif
