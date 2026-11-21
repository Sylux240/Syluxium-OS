// SPDX-License-Identifier: GPL-2.0
/*
 * LinuxKPI: sockets of address families implemented by Linux code.
 *
 * Families register with sock_register(). The Syluxium socket layer
 * (kernel/linuxkpi/socket.cpp) creates a struct socket for socket(2) and
 * calls the family's proto_ops. struct sock here carries what those
 * families use: receive queue, state, wait queue, the lock_sock() owner
 * lock, callbacks. No memory accounting: buffers are not charged.
 */
#include <linux/lkpi_all.h>
#include <linux/net.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <linux/poll.h>

/* ---- families ---- */

static const struct net_proto_family *families[AF_MAX];
static DEFINE_SPINLOCK(families_lock);

int sock_register(const struct net_proto_family *fam)
{
	int err = 0;
	if (fam->family < 0 || fam->family >= AF_MAX)
		return -ENOBUFS;
	spin_lock(&families_lock);
	if (families[fam->family])
		err = -EEXIST;
	else
		families[fam->family] = fam;
	spin_unlock(&families_lock);
	if (!err)
		printk(KERN_INFO "NET: Registered %s protocol family\n", fam->family == AF_BLUETOOTH ? "PF_BLUETOOTH" : "a");
	return err;
}

void sock_unregister(int family)
{
	spin_lock(&families_lock);
	if (family >= 0 && family < AF_MAX)
		families[family] = NULL;
	spin_unlock(&families_lock);
}

const struct net_proto_family *lkpi_sock_family(int family)
{
	const struct net_proto_family *f = NULL;
	spin_lock(&families_lock);
	if (family >= 0 && family < AF_MAX)
		f = families[family];
	spin_unlock(&families_lock);
	return f;
}

struct socket *sock_alloc(void)
{
	struct socket *sock = kzalloc(sizeof(*sock), GFP_KERNEL);
	if (!sock)
		return NULL;
	init_waitqueue_head(&sock->wq.wait);
	sock->state = SS_UNCONNECTED;
	return sock;
}

void sock_release(struct socket *sock)
{
	if (!sock)
		return;
	if (sock->ops && sock->ops->release)
		sock->ops->release(sock);
	sock->ops = NULL;
	kfree(sock);
}

int sock_create_kern(struct net *net, int family, int type, int proto, struct socket **res)
{
	const struct net_proto_family *f = lkpi_sock_family(family);
	struct socket *sock;
	int err;
	*res = NULL;
	if (!f)
		return -EAFNOSUPPORT;
	sock = sock_alloc();
	if (!sock)
		return -ENOMEM;
	sock->type = (short)type;
	err = f->create(net ? net : &init_net, sock, proto, 1);
	if (err) {
		kfree(sock);
		return err;
	}
	*res = sock;
	return 0;
}

int sock_sendmsg(struct socket *sock, struct msghdr *msg)
{
	return sock->ops->sendmsg(sock, msg, msg_data_left(msg));
}

int sock_recvmsg(struct socket *sock, struct msghdr *msg, int flags)
{
	return sock->ops->recvmsg(sock, msg, msg_data_left(msg), flags);
}

int kernel_sendmsg(struct socket *sock, struct msghdr *msg, struct kvec *vec, size_t num, size_t len)
{
	iov_iter_kvec(&msg->msg_iter, ITER_SOURCE, vec, num, len);
	return sock_sendmsg(sock, msg);
}

int kernel_recvmsg(struct socket *sock, struct msghdr *msg, struct kvec *vec, size_t num, size_t len, int flags)
{
	iov_iter_kvec(&msg->msg_iter, ITER_DEST, vec, num, len);
	return sock_recvmsg(sock, msg, flags);
}

int kernel_bind(struct socket *sock, struct sockaddr *addr, int addrlen) { return sock->ops->bind(sock, addr, addrlen); }
int kernel_listen(struct socket *sock, int backlog) { return sock->ops->listen(sock, backlog); }
int kernel_connect(struct socket *sock, struct sockaddr *addr, int addrlen, int flags)
{
	return sock->ops->connect(sock, addr, addrlen, flags);
}
int kernel_getsockname(struct socket *sock, struct sockaddr *addr) { return sock->ops->getname(sock, addr, 0); }
int kernel_getpeername(struct socket *sock, struct sockaddr *addr) { return sock->ops->getname(sock, addr, 1); }
int kernel_sock_shutdown(struct socket *sock, int how) { return sock->ops->shutdown(sock, how); }

int kernel_accept(struct socket *sock, struct socket **newsock, int flags)
{
	struct proto_accept_arg arg = { .flags = flags, .kern = true };
	struct socket *n = sock_alloc();
	int err;
	if (!n)
		return -ENOMEM;
	n->type = sock->type;
	n->ops = sock->ops;
	err = sock->ops->accept(sock, n, &arg);
	if (err) {
		kfree(n);
		return err;
	}
	*newsock = n;
	return 0;
}

/* ---- proto_ops fillers ---- */

int sock_no_bind(struct socket *s, struct sockaddr *a, int l) { return -EOPNOTSUPP; }
int sock_no_connect(struct socket *s, struct sockaddr *a, int l, int f) { return -EOPNOTSUPP; }
int sock_no_socketpair(struct socket *a, struct socket *b) { return -EOPNOTSUPP; }
int sock_no_accept(struct socket *s, struct socket *n, struct proto_accept_arg *arg) { return -EOPNOTSUPP; }
int sock_no_getname(struct socket *s, struct sockaddr *a, int p) { return -EOPNOTSUPP; }
int sock_no_ioctl(struct socket *s, unsigned int c, unsigned long a) { return -EOPNOTSUPP; }
int sock_no_listen(struct socket *s, int b) { return -EOPNOTSUPP; }
int sock_no_shutdown(struct socket *s, int h) { return -EOPNOTSUPP; }
int sock_no_sendmsg(struct socket *s, struct msghdr *m, size_t l) { return -EOPNOTSUPP; }
int sock_no_sendmsg_locked(struct sock *sk, struct msghdr *m, size_t l) { return -EOPNOTSUPP; }
int sock_no_recvmsg(struct socket *s, struct msghdr *m, size_t l, int f) { return -EOPNOTSUPP; }
int sock_no_mmap(struct file *file, struct socket *sock, struct vm_area_struct *vma) { return -ENODEV; }
int sock_common_setsockopt(struct socket *sock, int level, int optname, sockptr_t optval, unsigned int optlen)
{
	return -ENOPROTOOPT;
}
int sock_common_getsockopt(struct socket *sock, int level, int optname, char __user *optval, int __user *optlen)
{
	return -ENOPROTOOPT;
}

/* ---- struct sock ---- */

int proto_register(struct proto *prot, int alloc_slab) { return 0; }
void proto_unregister(struct proto *prot) { }

static unsigned long next_ino = 1000;

static void sock_def_wakeup(struct sock *sk)
{
	wake_up_interruptible_all(sk_sleep(sk));
	if (sk->sk_socket)
		lkpi_sock_notify(sk->sk_socket);
}

void sock_def_readable(struct sock *sk)
{
	wake_up_interruptible_all(sk_sleep(sk));
	if (sk->sk_socket)
		lkpi_sock_notify(sk->sk_socket);
}

static void sock_def_write_space(struct sock *sk) { sock_def_wakeup(sk); }
static void sock_def_error_report(struct sock *sk) { sock_def_wakeup(sk); }
static void sock_def_destruct(struct sock *sk) { }

struct sock *sk_alloc(struct net *net, int family, gfp_t priority, struct proto *prot, int kern)
{
	unsigned int size = prot && prot->obj_size > sizeof(struct sock) ? prot->obj_size : sizeof(struct sock);
	struct sock *sk = kzalloc(size, priority);
	if (!sk)
		return NULL;
	sk->sk_family = (unsigned short)family;
	sk->sk_prot = prot;
	write_pnet(&sk->sk_net, net ? net : &init_net);
	refcount_set(&sk->sk_refcnt, 1);
	INIT_HLIST_NODE(&sk->sk_node);
	skb_queue_head_init(&sk->sk_receive_queue);
	skb_queue_head_init(&sk->sk_write_queue);
	skb_queue_head_init(&sk->sk_error_queue);
	init_waitqueue_head(&sk->lkpi_wq.wait);
	mutex_init(&sk->lkpi_owner);
	spin_lock_init(&sk->lkpi_slock);
	spin_lock_init(&sk->sk_peer_lock);
	rwlock_init(&sk->sk_callback_lock);
	sk->sk_destruct = sock_def_destruct;
	sk->lkpi_ino = __atomic_add_fetch(&next_ino, 1, __ATOMIC_RELAXED);
	return sk;
}

void sk_free(struct sock *sk)
{
	if (!sk)
		return;
	if (sk->sk_destruct)
		sk->sk_destruct(sk);
	skb_queue_purge(&sk->sk_receive_queue);
	skb_queue_purge(&sk->sk_write_queue);
	skb_queue_purge(&sk->sk_error_queue);
	kfree(sk);
}

void sock_init_data(struct socket *sock, struct sock *sk)
{
	sk->sk_state = TCP_CLOSE;
	sk->sk_rcvbuf = SK_RMEM_MAX;
	sk->sk_sndbuf = SK_WMEM_MAX;
	sk->sk_rcvtimeo = MAX_SCHEDULE_TIMEOUT;
	sk->sk_sndtimeo = MAX_SCHEDULE_TIMEOUT;
	sk->sk_rcvlowat = 1;
	sk->sk_state_change = sock_def_wakeup;
	sk->sk_data_ready = sock_def_readable;
	sk->sk_write_space = sock_def_write_space;
	sk->sk_error_report = sock_def_error_report;
	refcount_set(&sk->sk_wmem_alloc, 1);
	if (sock) {
		sk->sk_type = sock->type;
		sock_graft(sk, sock);
	}
}

kuid_t sock_i_uid(struct sock *sk) { return current_euid(); }

void lock_sock_nested(struct sock *sk, int subclass)
{
	might_sleep();
	mutex_lock(&sk->lkpi_owner);
}

void release_sock(struct sock *sk)
{
	mutex_unlock(&sk->lkpi_owner);
}

/* ---- receiving ---- */

int sock_queue_rcv_skb(struct sock *sk, struct sk_buff *skb)
{
	if (sock_flag(sk, SOCK_DEAD))
		return -ENOTCONN;
	skb->sk = sk;
	skb->dev = NULL;
	skb_queue_tail(&sk->sk_receive_queue, skb);
	if (!sock_flag(sk, SOCK_DEAD))
		sk->sk_data_ready(sk);
	return 0;
}

struct sk_buff *sock_alloc_send_pskb(struct sock *sk, unsigned long header_len, unsigned long data_len,
				     int noblock, int *errcode, int max_page_order)
{
	struct sk_buff *skb;
	int err = sock_error(sk);
	if (err) {
		*errcode = err;
		return NULL;
	}
	if (sk->sk_shutdown & SEND_SHUTDOWN) {
		*errcode = -EPIPE;
		return NULL;
	}
	skb = alloc_skb(header_len + data_len, sk->sk_socket ? GFP_KERNEL : GFP_ATOMIC);
	if (!skb) {
		*errcode = -ENOBUFS;
		return NULL;
	}
	skb_set_owner_w(skb, sk);
	*errcode = 0;
	return skb;
}

struct sk_buff *sock_alloc_send_skb(struct sock *sk, unsigned long size, int noblock, int *errcode)
{
	return sock_alloc_send_pskb(sk, size, 0, noblock, errcode, 0);
}

/* The next datagram (blocking unless MSG_DONTWAIT). With MSG_PEEK it stays
 * queued and the caller gets a reference. */
struct sk_buff *skb_recv_datagram(struct sock *sk, unsigned int flags, int *err)
{
	long timeo = sock_rcvtimeo(sk, flags & MSG_DONTWAIT);
	for (;;) {
		struct sk_buff *skb;
		unsigned long irqflags;
		spin_lock_irqsave(&sk->sk_receive_queue.lock, irqflags);
		skb = skb_peek(&sk->sk_receive_queue);
		if (skb) {
			if (flags & MSG_PEEK)
				refcount_inc(&skb->users);
			else
				__skb_unlink(skb, &sk->sk_receive_queue);
		}
		spin_unlock_irqrestore(&sk->sk_receive_queue.lock, irqflags);
		if (skb) {
			*err = 0;
			return skb;
		}
		*err = sock_error(sk);
		if (*err)
			return NULL;
		if (sk->sk_shutdown & RCV_SHUTDOWN) {
			*err = 0;
			return NULL;
		}
		if (!timeo) {
			*err = -EAGAIN;
			return NULL;
		}
		{
			DEFINE_WAIT_FUNC(wait, woken_wake_function);
			add_wait_queue(sk_sleep(sk), &wait);
			if (skb_queue_empty(&sk->sk_receive_queue) && !sk->sk_err && !(sk->sk_shutdown & RCV_SHUTDOWN))
				timeo = wait_woken(&wait, TASK_INTERRUPTIBLE, timeo);
			remove_wait_queue(sk_sleep(sk), &wait);
		}
		if (signal_pending(current)) {
			*err = sock_intr_errno(timeo);
			return NULL;
		}
	}
}

void skb_free_datagram(struct sock *sk, struct sk_buff *skb) { consume_skb(skb); }

int lkpi_skb_copy_to_iter(const struct sk_buff *from, int offset, struct iov_iter *to, int size)
{
	if (offset < 0 || size < 0 || offset + size > (int)from->len)
		return -EFAULT;
	return copy_to_iter(from->data + offset, size, to) == (size_t)size ? 0 : -EFAULT;
}

int skb_copy_datagram_msg(const struct sk_buff *from, int offset, struct msghdr *msg, int size)
{
	return lkpi_skb_copy_to_iter(from, offset, &msg->msg_iter, size);
}

__poll_t datagram_poll(struct file *file, struct socket *sock, struct poll_table_struct *wait)
{
	struct sock *sk = sock->sk;
	__poll_t mask = 0;
	if (sk->sk_err || !skb_queue_empty(&sk->sk_error_queue))
		mask |= EPOLLERR;
	if (sk->sk_shutdown & RCV_SHUTDOWN)
		mask |= EPOLLRDHUP | EPOLLIN | EPOLLRDNORM;
	if (sk->sk_shutdown == SHUTDOWN_MASK)
		mask |= EPOLLHUP;
	if (!skb_queue_empty(&sk->sk_receive_queue))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (sock_writeable(sk))
		mask |= EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND;
	return mask;
}

/* ---- the Syluxium side (socket.cpp) calls these: struct socket stays here ---- */

void lkpi_sock_notify_native(void *native);

void lkpi_sock_notify(struct socket *sock)
{
	lkpi_sock_notify_native(sock ? sock->lkpi_native : NULL);
}

void *lkpi_socket_create(int family, int type, int protocol, long *err)
{
	const struct net_proto_family *f = lkpi_sock_family(family);
	struct socket *sock;
	int r;
	if (!f) {
		*err = -EAFNOSUPPORT;
		return NULL;
	}
	sock = sock_alloc();
	if (!sock) {
		*err = -ENOMEM;
		return NULL;
	}
	sock->type = (short)type;
	r = f->create(&init_net, sock, protocol, 0);
	if (r) {
		kfree(sock);
		*err = r;
		return NULL;
	}
	return sock;
}

void lkpi_socket_set_native(void *s, void *native) { ((struct socket *)s)->lkpi_native = native; }
void *lkpi_socket_get_native(void *s) { return ((struct socket *)s)->lkpi_native; }

long lkpi_socket_bind(void *s, const void *addr, int len)
{
	struct socket *sock = s;
	return sock->ops->bind(sock, (struct sockaddr *)addr, len);
}

long lkpi_socket_connect(void *s, const void *addr, int len, int nonblock)
{
	struct socket *sock = s;
	return sock->ops->connect(sock, (struct sockaddr *)addr, len, nonblock ? O_NONBLOCK : 0);
}

long lkpi_socket_listen(void *s, int backlog)
{
	struct socket *sock = s;
	return sock->ops->listen(sock, backlog);
}

long lkpi_socket_accept(void *s, void **out, int nonblock)
{
	return kernel_accept(s, (struct socket **)out, nonblock ? O_NONBLOCK : 0);
}

long lkpi_socket_sendmsg(void *s, const void *data, size_t len, const void *name, int namelen, int flags)
{
	struct socket *sock = s;
	struct kvec v = { (void *)data, len };
	struct msghdr msg = {};
	char addr[128];
	if (name && namelen > 0) {
		if (namelen > (int)sizeof(addr))
			return -EINVAL;
		memcpy(addr, name, namelen);
		msg.msg_name = addr;
		msg.msg_namelen = namelen;
	}
	msg.msg_flags = flags;
	iov_iter_kvec(&msg.msg_iter, ITER_SOURCE, &v, 1, len);
	return sock->ops->sendmsg(sock, &msg, len);
}

long lkpi_socket_recvmsg(void *s, void *buf, size_t len, void *name, int *namelen, int flags)
{
	struct socket *sock = s;
	struct kvec v = { buf, len };
	struct msghdr msg = {};
	long r;
	msg.msg_name = name;
	iov_iter_kvec(&msg.msg_iter, ITER_DEST, &v, 1, len);
	r = sock->ops->recvmsg(sock, &msg, len, flags);
	*namelen = msg.msg_namelen;
	return r;
}

long lkpi_socket_getname(void *s, void *addr, int peer)
{
	struct socket *sock = s;
	return sock->ops->getname(sock, addr, peer);
}

long lkpi_socket_shutdown(void *s, int how)
{
	struct socket *sock = s;
	return sock->ops->shutdown(sock, how);
}

unsigned lkpi_socket_poll(void *s)
{
	struct socket *sock = s;
	return sock->ops->poll ? sock->ops->poll(NULL, sock, NULL) : 0;
}

long lkpi_socket_setsockopt(void *s, int level, int name, const void *val, unsigned len)
{
	struct socket *sock = s;
	if (!sock->ops->setsockopt)
		return -ENOPROTOOPT;
	return sock->ops->setsockopt(sock, level, name, KERNEL_SOCKPTR((void *)val), len);
}

/* getsockopt writes through "user" pointers: here they are kernel buffers. */
long lkpi_socket_getsockopt(void *s, int level, int name, void *val, int *len)
{
	struct socket *sock = s;
	long r;
	if (!sock->ops->getsockopt)
		return -ENOPROTOOPT;
	current->lkpi_kernel_uaccess++;
	r = sock->ops->getsockopt(sock, level, name, val, len);
	current->lkpi_kernel_uaccess--;
	return r;
}

long lkpi_socket_ioctl(void *s, unsigned cmd, unsigned long arg)
{
	struct socket *sock = s;
	return sock->ops->ioctl ? sock->ops->ioctl(sock, cmd, arg) : -ENOIOCTLCMD;
}

void lkpi_socket_release(void *s) { sock_release(s); }
