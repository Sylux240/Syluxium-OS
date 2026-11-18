// BSD socket API (Linux ABI): socket, bind, connect, listen, accept,
// send/recv families (with control messages), socket options, and the
// interface ioctls that ifconfig/route use.

#include <core/log.hpp>
#include <fs/poll.hpp>
#include <lib/format.hpp>
#include <mm/heap.hpp>
#include <net/internal.hpp>
#include <proc/process.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>
#include <syscall/syscall.hpp>

namespace net {

namespace {

constexpr usize CHUNK = 64 * 1024; // kernel bounce buffer per call
constexpr long EOPNOTSUPP = 95;

} // namespace

Socket::Socket(int t, int p, u16 d) : type(t), protocol(p), domain(d) {
    ino = vfs::next_ino();
    mode = vfs::S_IFSOCK | 0777;
    atime = mtime = ctime = vfs::now();
}

long Socket::bind(const Endpoint&) { return -EOPNOTSUPP; }
long Socket::connect(const Endpoint&, bool) { return -EOPNOTSUPP; }
long Socket::listen(int) { return -EOPNOTSUPP; }
long Socket::accept(Socket**, bool) { return -EOPNOTSUPP; }
long Socket::shutdown(int) { return -ENOTCONN; }

void Socket::wake() {
    wq.wake_all();
    poll::notify();
}

// ---- address translation ----

long sockaddr_to_endpoint(const SockAddr& sa, Endpoint* out, u16 domain) {
    *out = Endpoint{};
    if (domain == AF_INET6) {
        if (sa.family() != AF_INET6)
            return -EAFNOSUPPORT;
        if (sa.len < 24) // sin6_scope_id is optional (RFC 2133 layout)
            return -err::INVAL;
        SockAddrIn6 in6{};
        memcpy(&in6, sa.data, min<u32>(sa.len, sizeof(in6)));
        out->port = be16(in6.port);
        if (ip6::is_v4mapped(in6.addr)) {
            out->addr = get32(in6.addr + 12);
        } else {
            out->v6 = true;
            memcpy(out->addr6, in6.addr, 16);
            out->scope = sa.len >= sizeof(in6) ? in6.scope_id : 0;
        }
        return 0;
    }
    if (sa.family() != AF_INET)
        return -EAFNOSUPPORT;
    if (sa.len < sizeof(SockAddrIn))
        return -err::INVAL;
    SockAddrIn in;
    memcpy(&in, sa.data, sizeof(in));
    out->addr = be32(in.addr);
    out->port = be16(in.port);
    return 0;
}

void endpoint_to_sockaddr(const Endpoint& ep, SockAddr* out, u16 domain) {
    memset(out->data, 0, sizeof(out->data));
    if (domain == AF_INET6) {
        SockAddrIn6 in6{};
        in6.family = AF_INET6;
        in6.port = be16(ep.port);
        if (ep.v6) {
            memcpy(in6.addr, ep.addr6, 16);
            in6.scope_id = ip6::is_link_local(ep.addr6) ? ep.scope : 0;
        } else if (ep.addr) { // v4-mapped; 0.0.0.0 stays ::
            in6.addr[10] = in6.addr[11] = 0xFF;
            put32(in6.addr + 12, ep.addr);
        }
        memcpy(out->data, &in6, sizeof(in6));
        out->len = sizeof(in6);
        return;
    }
    SockAddrIn in{};
    in.family = AF_INET;
    in.port = be16(ep.port);
    in.addr = be32(ep.addr);
    memcpy(out->data, &in, sizeof(in));
    out->len = sizeof(in);
}

bool local_accepts(const Socket* s, const Endpoint& dst) {
    if (dst.v6 && s->domain != AF_INET6)
        return false;
    if (!dst.v6 && s->domain == AF_INET6 && s->v6only)
        return false;
    if (s->local.any()) // a v4-mapped wildcard (::ffff:0.0.0.0) only takes IPv4
        return !(dst.v6 && !s->local.v6);
    return s->local.same_addr(dst);
}

bool bindings_overlap(const Endpoint& a, const Endpoint& b) {
    if (a.any() || b.any())
        return true;
    return a.same_addr(b);
}

// ---- default (Internet) implementations of the generic entry points ----

long Socket::bind_addr(const SockAddr& sa) {
    Endpoint ep;
    if (long r = sockaddr_to_endpoint(sa, &ep, domain); r < 0)
        return r;
    return bind(ep);
}

long Socket::connect_addr(const SockAddr& sa, bool nonblock) {
    Endpoint ep;
    if (sa.family() == AF_UNSPEC) // dissolve a datagram association
        return connect(Endpoint{}, nonblock);
    if (long r = sockaddr_to_endpoint(sa, &ep, domain); r < 0)
        return r;
    return connect(ep, nonblock);
}

long Socket::get_name(SockAddr* out, bool peer) {
    if (peer) {
        if (!connected)
            return -ENOTCONN;
        endpoint_to_sockaddr(remote, out, domain);
        return 0;
    }
    Endpoint ep = local;
    if (ep.any() && connected) {
        LockGuard guard(g_lock);
        Endpoint src;
        if (source_endpoint(remote, &src)) {
            src.port = ep.port;
            ep = src;
        }
    }
    endpoint_to_sockaddr(ep, out, domain);
    return 0;
}

isize Socket::send_msg(const u8* data, usize len, int flags, const SockAddr* to, Control*, bool nonblock) {
    Endpoint ep;
    if (to) {
        if (long r = sockaddr_to_endpoint(*to, &ep, domain); r < 0)
            return r;
    }
    return send(data, len, flags, to ? &ep : nullptr, nonblock);
}

isize Socket::recv_msg(u8* buf, usize len, int flags, SockAddr* from, Control*, bool nonblock, usize* full_len) {
    Endpoint ep = remote;
    isize r = recv(buf, len, flags, &ep, nonblock, full_len);
    if (r >= 0 && from)
        endpoint_to_sockaddr(ep, from, domain);
    return r;
}

// ---- control messages ----

void release_control(Control& c) {
    for (int i = 0; i < c.nfiles; i++)
        if (c.files[i])
            vfs::file_put(c.files[i]);
    kfree(c.files);
    c.files = nullptr;
    c.nfiles = 0;
}

Ucred current_cred() {
    Ucred c;
    c.pid = proc::current() ? proc::current()->pid : 0;
    c.uid = proc::cred().euid; // what SO_PEERCRED / SCM_CREDENTIALS report (D-Bus checks them)
    c.gid = proc::cred().egid;
    return c;
}

// ---- read()/write() on a socket = recv/send without flags ----

isize Socket::read_file(vfs::File* f, u64, void* buf, usize count) {
    usize n = min(count, CHUNK);
    u8* k = static_cast<u8*>(kmalloc(n ? n : 1));
    if (!k)
        return -err::NOMEM;
    usize full;
    isize r = recv_msg(k, n, 0, nullptr, nullptr, f->flags & vfs::O_NONBLOCK, &full);
    if (r > 0)
        memcpy(buf, k, static_cast<usize>(r));
    kfree(k);
    return r;
}

isize Socket::write_file(vfs::File* f, u64, const void* buf, usize count) {
    usize done = 0;
    u8* k = static_cast<u8*>(kmalloc(min(count, CHUNK) ? min(count, CHUNK) : 1));
    if (!k)
        return -err::NOMEM;
    while (done < count || count == 0) {
        usize n = min(count - done, CHUNK);
        memcpy(k, static_cast<const u8*>(buf) + done, n);
        isize r = send_msg(k, n, 0, nullptr, nullptr, f->flags & vfs::O_NONBLOCK);
        if (r < 0) {
            kfree(k);
            return done ? static_cast<isize>(done) : r;
        }
        done += static_cast<usize>(r);
        if (count == 0 || static_cast<usize>(r) < n || type != SOCK_STREAM)
            break;
    }
    kfree(k);
    return static_cast<isize>(done);
}

void Socket::on_release(vfs::File*) { close(); }

// ---- interface ioctls (struct ifreq: 16-byte name + 24-byte union) ----

namespace {

struct IfReq {
    char name[16];
    union {
        SockAddrIn addr;
        u16 flags;
        i32 ivalue;
        struct {
            u16 family;
            u8 data[14];
        } hwaddr;
        char newname[16];
        u8 pad[24];
    };
};
static_assert(sizeof(IfReq) == 40);

void put_addr(SockAddrIn* sa, u32 ip) {
    memset(sa, 0, sizeof(*sa));
    sa->family = AF_INET;
    sa->addr = be32(ip);
}

} // namespace

long Socket::ioctl(u64 request, u64 arg) {
    void* uarg = reinterpret_cast<void*>(arg);
    switch (request) {
    case 0x541B: { // FIONREAD
        i32 n = static_cast<i32>(bytes_available());
        return uaccess::put(static_cast<i32*>(uarg), n);
    }
    case 0x5421: // FIONBIO: honoured through fcntl by every libc; accept it
        return 0;
    case 0x8912: { // SIOCGIFCONF
        struct IfConf {
            i32 len;
            i32 pad;
            u64 buf;
        } conf;
        if (long r = uaccess::get(&conf, static_cast<IfConf*>(uarg)); r < 0)
            return r;
        LockGuard guard(g_lock);
        i32 used = 0;
        for (u32 i = 1; interface_by_index(i); i++) {
            Interface* f = interface_by_index(i);
            if (!f->addr)
                continue;
            if (conf.buf) {
                if (used + static_cast<i32>(sizeof(IfReq)) > conf.len)
                    break;
                IfReq r{};
                snprintf(r.name, sizeof(r.name), "%s", f->name);
                put_addr(&r.addr, f->addr);
                if (long e = uaccess::copy_to(reinterpret_cast<void*>(conf.buf + used), &r, sizeof(r)); e < 0)
                    return e;
            }
            used += sizeof(IfReq);
        }
        conf.len = used;
        return uaccess::put(static_cast<IfConf*>(uarg), conf);
    }
    case 0x890B: case 0x890C: { // SIOCADDRT / SIOCDELRT (struct rtentry)
        u8 rt[120];
        if (long r = uaccess::copy_from(rt, uarg, sizeof(rt)); r < 0)
            return r;
        SockAddrIn dst, gw;
        memcpy(&dst, rt + 8, 16);
        memcpy(&gw, rt + 24, 16);
        LockGuard guard(g_lock);
        if (dst.addr != 0)
            return 0; // only the default route matters for us; others are implied by netmasks
        set_gateway(request == 0x890B ? be32(gw.addr) : 0);
        return 0;
    }
    default:
        break;
    }
    if ((request & 0xFF00) != 0x8900)
        return -err::NOTTY;

    IfReq req;
    if (long r = uaccess::get(&req, static_cast<IfReq*>(uarg)); r < 0)
        return r;
    req.name[15] = 0;
    LockGuard guard(g_lock);
    Interface* f = nullptr;
    if (request == 0x8910) { // SIOCGIFNAME: by index
        f = interface_by_index(static_cast<u32>(req.ivalue));
        if (!f)
            return -err::NODEV;
        snprintf(req.name, sizeof(req.name), "%s", f->name);
        return uaccess::put(static_cast<IfReq*>(uarg), req);
    }
    f = interface_by_name(req.name);
    if (!f)
        return -err::NODEV;
    switch (request) {
    case 0x8913: req.flags = static_cast<u16>(f->flags | (f->link_up ? IFF_RUNNING : 0)); break; // SIOCGIFFLAGS
    case 0x8914: f->flags = (f->flags & IFF_LOOPBACK) | (req.flags & ~IFF_LOOPBACK); return 0;     // SIOCSIFFLAGS
    case 0x8915: put_addr(&req.addr, f->addr); break;                                             // SIOCGIFADDR
    case 0x8916: f->addr = be32(req.addr.addr); return 0;                                         // SIOCSIFADDR
    case 0x8917: put_addr(&req.addr, 0); break;                                                   // SIOCGIFDSTADDR
    case 0x8919: put_addr(&req.addr, f->addr | ~f->netmask); break;                              // SIOCGIFBRDADDR
    case 0x891A: return 0;                                                                         // SIOCSIFBRDADDR
    case 0x891B: put_addr(&req.addr, f->netmask); break;                                          // SIOCGIFNETMASK
    case 0x891C: f->netmask = be32(req.addr.addr); return 0;                                      // SIOCSIFNETMASK
    case 0x891D: req.ivalue = 0; break;                                                            // SIOCGIFMETRIC
    case 0x8921: req.ivalue = static_cast<i32>(f->mtu); break;                                    // SIOCGIFMTU
    case 0x8922: if (req.ivalue < 68) return -err::INVAL; f->mtu = static_cast<u32>(req.ivalue); return 0;
    case 0x8927:                                                                                   // SIOCGIFHWADDR
        memset(&req.hwaddr, 0, sizeof(req.hwaddr));
        req.hwaddr.family = (f->flags & IFF_LOOPBACK) ? 772 : 1; // ARPHRD_LOOPBACK / ARPHRD_ETHER
        memcpy(req.hwaddr.data, f->mac, 6);
        break;
    case 0x8933: req.ivalue = static_cast<i32>(f->index); break;                                  // SIOCGIFINDEX
    case 0x8942: req.ivalue = 1000; break;                                                         // SIOCGIFTXQLEN
    case 0x8970: memset(req.pad, 0, sizeof(req.pad)); break;                                      // SIOCGIFMAP
    default: return -EOPNOTSUPP;
    }
    return uaccess::put(static_cast<IfReq*>(uarg), req);
}

} // namespace net

// ---- system calls ----

namespace syscall {

namespace {

using net::Socket;
using namespace net;

constexpr long EPROTONOSUPPORT = 93, ENOTSOCK = 88, EOPNOTSUPP = 95;
constexpr u32 SOCK_NONBLOCK = 04000, SOCK_CLOEXEC = 02000000;
constexpr usize CHUNK_SIZE = 64 * 1024;
constexpr int MAX_PASSED_FDS = 253; // Linux's SCM_MAX_FD

class SockRef {
public:
    SockRef(long fd) : m_file(fd >= 0 ? proc::fd_get(proc::current(), static_cast<int>(fd)) : nullptr) {
        if (m_file && (m_file->inode->mode & vfs::S_IFMT) == vfs::S_IFSOCK)
            m_sock = static_cast<Socket*>(m_file->inode);
    }
    ~SockRef() {
        if (m_file)
            vfs::file_put(m_file);
    }
    long error() const { return !m_file ? -err::BADF : !m_sock ? -ENOTSOCK : 0; }
    Socket* operator->() const { return m_sock; }
    Socket* get() const { return m_sock; }
    bool nonblock() const { return m_file->flags & vfs::O_NONBLOCK; }

private:
    vfs::File* m_file;
    Socket* m_sock = nullptr;
};

// Copy a user sockaddr in as-is; the socket interprets it.
long read_addr(u64 uaddr, u64 len, SockAddr* out) {
    if (len > sizeof(out->data))
        len = sizeof(out->data); // longer sockaddr_storage: the tail is padding
    if (len < 2 && len != 0)
        return -err::INVAL;
    *out = SockAddr{};
    if (long r = uaccess::copy_from(out->data, reinterpret_cast<void*>(uaddr), len); r < 0)
        return r;
    out->len = static_cast<u32>(len);
    return 0;
}

// Store an address for the caller, truncated to its buffer; *ulen gets the
// real length (as Linux does).
long write_addr(const SockAddr& sa, u64 uaddr, u64 ulen_ptr) {
    if (!uaddr || !ulen_ptr)
        return 0;
    u32 len;
    if (long r = uaccess::get(&len, reinterpret_cast<u32*>(ulen_ptr)); r < 0)
        return r;
    if (long r = uaccess::copy_to(reinterpret_cast<void*>(uaddr), sa.data, min<u32>(len, sa.len)); r < 0)
        return r;
    return uaccess::put(reinterpret_cast<u32*>(ulen_ptr), sa.len);
}

long install(Socket* s, u32 flags) {
    vfs::File* f = vfs::open_inode(s, vfs::O_RDWR | ((flags & SOCK_NONBLOCK) ? vfs::O_NONBLOCK : 0));
    char name[48];
    snprintf(name, sizeof(name), "socket:[%lu]", s->ino);
    vfs::file_set_path(f, name);
    long fd = proc::fd_install(proc::current(), f, 0, flags & SOCK_CLOEXEC);
    if (fd < 0)
        vfs::file_put(f);
    return fd;
}

long sys_socket(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    int domain = static_cast<int>(a0);
    int type = static_cast<int>(a1 & 0xF);
    int protocol = static_cast<int>(a2);
    Socket* s = nullptr;
    switch (domain) {
    case AF_UNIX:
        if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_SEQPACKET)
            return -EPROTONOSUPPORT;
        if (protocol != 0 && protocol != AF_UNIX)
            return -EPROTONOSUPPORT;
        s = create_unix(type);
        break;
    case AF_NETLINK:
        if (type != SOCK_RAW && type != SOCK_DGRAM)
            return -EPROTONOSUPPORT;
        // Route (0) is answered; uevent listeners (15) just hear nothing.
        // Protocols whose users wait for answers (audit: PAM/login, sock_diag,
        // generic netlink, ...) are refused, as by a kernel built without
        // them: libaudit then quietly skips auditing instead of timing out.
        if (protocol != 0 && protocol != 15)
            return -EPROTONOSUPPORT;
        s = create_netlink(type, protocol);
        break;
    case AF_INET:
        if (type == SOCK_STREAM && (protocol == 0 || protocol == PROTO_TCP))
            s = create_tcp();
        else if (type == SOCK_DGRAM && (protocol == 0 || protocol == PROTO_UDP))
            s = create_udp(type, PROTO_UDP);
        else if ((type == SOCK_DGRAM || type == SOCK_RAW) && protocol == PROTO_ICMP)
            s = create_icmp(type);
        else
            return -EPROTONOSUPPORT;
        break;
    case AF_INET6:
        if (type == SOCK_STREAM && (protocol == 0 || protocol == PROTO_TCP))
            s = create_tcp(AF_INET6);
        else if (type == SOCK_DGRAM && (protocol == 0 || protocol == PROTO_UDP))
            s = create_udp(type, PROTO_UDP, AF_INET6);
        else if ((type == SOCK_DGRAM || type == SOCK_RAW) && protocol == PROTO_ICMPV6)
            s = create_icmp6(type);
        else
            return -EPROTONOSUPPORT;
        break;
    default:
        return -EAFNOSUPPORT;
    }
    if (!s)
        return -err::NOMEM;
    return install(s, static_cast<u32>(a1));
}

long sys_socketpair(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    int type = static_cast<int>(a1 & 0xF);
    if (a0 != AF_UNIX)
        return a0 == AF_INET ? -EOPNOTSUPP : -EAFNOSUPPORT;
    if (a2 != 0 && a2 != AF_UNIX)
        return -EPROTONOSUPPORT;
    Socket *x, *y;
    if (long r = unix_socketpair(type, &x, &y); r < 0)
        return r;
    // install() closes the socket itself when it fails.
    long fx = install(x, static_cast<u32>(a1));
    if (fx < 0) {
        y->close();
        return fx;
    }
    long fy = install(y, static_cast<u32>(a1));
    if (fy < 0) {
        proc::fd_close(proc::current(), static_cast<int>(fx));
        return fy;
    }
    i32 fds[2] = {static_cast<i32>(fx), static_cast<i32>(fy)};
    if (long r = uaccess::copy_to(reinterpret_cast<void*>(a3), fds, sizeof(fds)); r < 0) {
        proc::fd_close(proc::current(), static_cast<int>(fx));
        proc::fd_close(proc::current(), static_cast<int>(fy));
        return r;
    }
    return 0;
}

long sys_bind(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    SockAddr sa;
    if (long r = read_addr(a1, a2, &sa); r < 0)
        return r;
    if (sa.len < 2)
        return -err::INVAL;
    return s->bind_addr(sa);
}

long sys_connect(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    SockAddr sa;
    if (long r = read_addr(a1, a2, &sa); r < 0)
        return r;
    if (sa.len < 2)
        return -err::INVAL;
    return s->connect_addr(sa, s.nonblock());
}

long sys_listen(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    return s->listen(static_cast<int>(a1));
}

long sys_accept4(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    Socket* child;
    if (long r = s->accept(&child, s.nonblock()); r < 0)
        return r;
    SockAddr peer;
    if (child->get_name(&peer, true) < 0)
        peer = SockAddr{};
    long fd = install(child, static_cast<u32>(a3));
    if (fd < 0)
        return fd;
    write_addr(peer, a1, a2);
    return fd;
}

long sys_accept(SYSCALL_ARGS) { return sys_accept4(a0, a1, a2, 0, a4, a5, frame); }

long sys_getsockname(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    SockAddr sa;
    if (long r = s->get_name(&sa, false); r < 0)
        return r;
    return write_addr(sa, a1, a2);
}

long sys_getpeername(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    SockAddr sa;
    if (long r = s->get_name(&sa, true); r < 0)
        return r;
    return write_addr(sa, a1, a2);
}

void maybe_sigpipe(isize r, int flags) {
    if (r == -err::PIPE && !(flags & MSG_NOSIGNAL))
        signal::send_thread(sched::current(), 13); // SIGPIPE
}

isize do_send(Socket* s, bool nonblock, const u8* user_buf, usize len, int flags, const SockAddr* to) {
    usize done = 0;
    u8* k = static_cast<u8*>(kmalloc(min(len, CHUNK_SIZE) ? min(len, CHUNK_SIZE) : 1));
    if (!k)
        return -err::NOMEM;
    do {
        usize n = min(len - done, CHUNK_SIZE);
        if (long r = uaccess::copy_from(k, user_buf + done, n); r < 0) {
            kfree(k);
            return r;
        }
        isize r = s->send_msg(k, n, flags, to, nullptr, nonblock || (flags & MSG_DONTWAIT));
        if (r < 0) {
            kfree(k);
            maybe_sigpipe(r, flags);
            return done ? static_cast<isize>(done) : r;
        }
        done += static_cast<usize>(r);
        if (static_cast<usize>(r) < n || s->type != SOCK_STREAM)
            break;
    } while (done < len);
    kfree(k);
    return static_cast<isize>(done);
}

long sys_sendto(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    SockAddr to;
    bool have_to = a4 != 0 && a5 != 0;
    if (have_to)
        if (long r = read_addr(a4, a5, &to); r < 0)
            return r;
    return do_send(s.get(), s.nonblock(), reinterpret_cast<const u8*>(a1), a2, static_cast<int>(a3),
                   have_to ? &to : nullptr);
}

isize do_recv(Socket* s, bool nonblock, u8* user_buf, usize len, int flags, SockAddr* from, usize* full) {
    usize n = min(len, CHUNK_SIZE);
    u8* k = static_cast<u8*>(kmalloc(n ? n : 1));
    if (!k)
        return -err::NOMEM;
    isize r = s->recv_msg(k, n, flags, from, nullptr, nonblock || (flags & MSG_DONTWAIT), full);
    if (r > 0)
        if (long e = uaccess::copy_to(user_buf, k, static_cast<usize>(r)); e < 0)
            r = e;
    kfree(k);
    return r;
}

long sys_recvfrom(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    SockAddr from;
    usize full = 0;
    isize r = do_recv(s.get(), s.nonblock(), reinterpret_cast<u8*>(a1), a2, static_cast<int>(a3), &from, &full);
    if (r < 0)
        return r;
    if (a4)
        write_addr(from, a4, a5);
    if ((a3 & MSG_TRUNC) && s->type != SOCK_STREAM)
        return static_cast<long>(full);
    return r;
}

struct MsgHdr {
    u64 name;
    u32 namelen;
    u32 pad;
    u64 iov;
    u64 iovlen;
    u64 control;
    u64 controllen;
    i32 flags;
    i32 pad2;
};
struct IoVec {
    u64 base;
    u64 len;
};
struct CmsgHdr {
    u64 len;
    i32 level;
    i32 type;
};
constexpr int SOL_SOCKET = 1, SCM_RIGHTS = 1, SCM_CREDENTIALS = 2;
constexpr u64 cmsg_align(u64 n) { return (n + 7) & ~7ull; }

// Gather a message's iovecs into one kernel buffer.
long gather(const MsgHdr& m, u8** out, usize* out_len) {
    if (m.iovlen > 1024)
        return -err::INVAL;
    usize total = 0;
    for (u64 i = 0; i < m.iovlen; i++) {
        IoVec v;
        if (long r = uaccess::get(&v, reinterpret_cast<IoVec*>(m.iov) + i); r < 0)
            return r;
        total += v.len;
    }
    if (total > CHUNK_SIZE)
        total = CHUNK_SIZE;
    u8* buf = static_cast<u8*>(kmalloc(total ? total : 1));
    if (!buf)
        return -err::NOMEM;
    usize pos = 0;
    for (u64 i = 0; i < m.iovlen && pos < total; i++) {
        IoVec v;
        uaccess::get(&v, reinterpret_cast<IoVec*>(m.iov) + i);
        usize n = min<usize>(v.len, total - pos);
        if (long r = uaccess::copy_from(buf + pos, reinterpret_cast<void*>(v.base), n); r < 0) {
            kfree(buf);
            return r;
        }
        pos += n;
    }
    *out = buf;
    *out_len = total;
    return 0;
}

long scatter(const MsgHdr& m, const u8* data, usize len) {
    usize pos = 0;
    for (u64 i = 0; i < m.iovlen && pos < len; i++) {
        IoVec v;
        if (long r = uaccess::get(&v, reinterpret_cast<IoVec*>(m.iov) + i); r < 0)
            return r;
        usize n = min<usize>(v.len, len - pos);
        if (long r = uaccess::copy_to(reinterpret_cast<void*>(v.base), data + pos, n); r < 0)
            return r;
        pos += n;
    }
    return 0;
}

// Parse sendmsg control messages: SCM_RIGHTS (take a reference on every
// file) and SCM_CREDENTIALS. Unknown messages are ignored, as Linux does
// for levels it doesn't know.
long parse_control(const MsgHdr& m, Control* ctl) {
    if (!m.control || m.controllen < sizeof(CmsgHdr))
        return 0;
    if (m.controllen > 64 * 1024)
        return -ENOBUFS;
    u64 pos = 0;
    while (pos + sizeof(CmsgHdr) <= m.controllen) {
        CmsgHdr h;
        if (long r = uaccess::get(&h, reinterpret_cast<CmsgHdr*>(m.control + pos)); r < 0)
            return r;
        if (h.len < sizeof(CmsgHdr) || pos + h.len > m.controllen)
            return -err::INVAL;
        u64 data = m.control + pos + sizeof(CmsgHdr);
        u64 dlen = h.len - sizeof(CmsgHdr);
        if (h.level == SOL_SOCKET && h.type == SCM_RIGHTS) {
            int n = static_cast<int>(dlen / 4);
            if (ctl->nfiles + n > MAX_PASSED_FDS)
                return -err::INVAL;
            auto** files = static_cast<vfs::File**>(kmalloc(sizeof(vfs::File*) * (ctl->nfiles + n + 1)));
            if (!files)
                return -err::NOMEM;
            for (int i = 0; i < ctl->nfiles; i++)
                files[i] = ctl->files[i];
            kfree(ctl->files);
            ctl->files = files;
            for (int i = 0; i < n; i++) {
                i32 fd;
                if (long r = uaccess::get(&fd, reinterpret_cast<i32*>(data) + i); r < 0)
                    return r;
                vfs::File* f = fd >= 0 ? proc::fd_get(proc::current(), fd) : nullptr;
                if (!f)
                    return -err::BADF;
                ctl->files[ctl->nfiles++] = f;
            }
        } else if (h.level == SOL_SOCKET && h.type == SCM_CREDENTIALS) {
            if (dlen < sizeof(Ucred))
                return -err::INVAL;
            if (long r = uaccess::copy_from(&ctl->cred, reinterpret_cast<void*>(data), sizeof(Ucred)); r < 0)
                return r;
            ctl->has_cred = true;
        }
        pos += cmsg_align(h.len);
    }
    return 0;
}

// Write received control messages into the caller's buffer: credentials
// first, then the passed files, each installed as a new descriptor. Files
// that don't fit are closed and MSG_CTRUNC is reported, like Linux.
long emit_control(const MsgHdr& m, Control& ctl, int flags, u64* used_out, int* msg_flags) {
    u64 used = 0;
    u64 room = m.control ? m.controllen : 0;
    if (ctl.has_cred) {
        u64 need = sizeof(CmsgHdr) + sizeof(Ucred);
        if (used + need <= room) {
            CmsgHdr h{need, SOL_SOCKET, SCM_CREDENTIALS};
            uaccess::put(reinterpret_cast<CmsgHdr*>(m.control + used), h);
            uaccess::copy_to(reinterpret_cast<void*>(m.control + used + sizeof(h)), &ctl.cred, sizeof(Ucred));
            used += cmsg_align(need);
        } else {
            *msg_flags |= MSG_CTRUNC;
        }
    }
    if (ctl.nfiles) {
        int fit = room >= used + sizeof(CmsgHdr) ? static_cast<int>((room - used - sizeof(CmsgHdr)) / 4) : 0;
        if (fit > ctl.nfiles)
            fit = ctl.nfiles;
        int installed = 0;
        for (int i = 0; i < fit; i++) {
            long fd = proc::fd_install(proc::current(), ctl.files[i], 0, flags & MSG_CMSG_CLOEXEC);
            if (fd < 0)
                break;
            ctl.files[i] = nullptr; // the descriptor table owns it now
            i32 v = static_cast<i32>(fd);
            uaccess::put(reinterpret_cast<i32*>(m.control + used + sizeof(CmsgHdr)) + installed, v);
            installed++;
        }
        if (installed) {
            CmsgHdr h{sizeof(CmsgHdr) + 4ull * installed, SOL_SOCKET, SCM_RIGHTS};
            uaccess::put(reinterpret_cast<CmsgHdr*>(m.control + used), h);
            used += cmsg_align(h.len);
        }
        if (installed < ctl.nfiles)
            *msg_flags |= MSG_CTRUNC;
    }
    release_control(ctl); // whatever wasn't installed
    *used_out = used;
    return 0;
}

long send_one_msg(Socket* s, bool nonblock, u64 umsg, int flags) {
    MsgHdr m;
    if (long r = uaccess::get(&m, reinterpret_cast<MsgHdr*>(umsg)); r < 0)
        return r;
    SockAddr to;
    bool have_to = m.name && m.namelen;
    if (have_to)
        if (long r = read_addr(m.name, m.namelen, &to); r < 0)
            return r;
    Control ctl;
    if (long r = parse_control(m, &ctl); r < 0) {
        release_control(ctl);
        return r;
    }
    u8* buf;
    usize len;
    if (long r = gather(m, &buf, &len); r < 0) {
        release_control(ctl);
        return r;
    }
    isize r = s->send_msg(buf, len, flags, have_to ? &to : nullptr, &ctl, nonblock || (flags & MSG_DONTWAIT));
    kfree(buf);
    release_control(ctl); // files the socket didn't take
    maybe_sigpipe(r, flags);
    return r;
}

long recv_one_msg(Socket* s, bool nonblock, u64 umsg, int flags) {
    MsgHdr m;
    if (long r = uaccess::get(&m, reinterpret_cast<MsgHdr*>(umsg)); r < 0)
        return r;
    usize total = 0;
    for (u64 i = 0; i < m.iovlen && i < 1024; i++) {
        IoVec v;
        if (long r = uaccess::get(&v, reinterpret_cast<IoVec*>(m.iov) + i); r < 0)
            return r;
        total += v.len;
    }
    total = min(total, CHUNK_SIZE);
    u8* buf = static_cast<u8*>(kmalloc(total ? total : 1));
    if (!buf)
        return -err::NOMEM;
    SockAddr from;
    Control ctl;
    usize full = 0;
    isize r = s->recv_msg(buf, total, flags, &from, &ctl, nonblock || (flags & MSG_DONTWAIT), &full);
    if (r >= 0) {
        if (long e = scatter(m, buf, static_cast<usize>(r)); e < 0)
            r = e;
    }
    kfree(buf);
    if (r < 0) {
        release_control(ctl);
        return r;
    }
    MsgHdr out = m;
    int msg_flags = (s->type != SOCK_STREAM && full > static_cast<usize>(r)) ? MSG_TRUNC : 0;
    u64 used = 0;
    emit_control(m, ctl, flags, &used, &msg_flags);
    out.controllen = used;
    out.flags = msg_flags;
    if (m.name && m.namelen) {
        uaccess::copy_to(reinterpret_cast<void*>(m.name), from.data, min<u32>(m.namelen, from.len));
        out.namelen = from.len;
    } else {
        out.namelen = 0;
    }
    uaccess::put(reinterpret_cast<MsgHdr*>(umsg), out);
    if ((flags & MSG_TRUNC) && s->type != SOCK_STREAM)
        return static_cast<long>(full);
    return r;
}

long sys_sendmsg(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    return send_one_msg(s.get(), s.nonblock(), a1, static_cast<int>(a2));
}

long sys_recvmsg(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    return recv_one_msg(s.get(), s.nonblock(), a1, static_cast<int>(a2));
}

// struct mmsghdr = msghdr + u32 msg_len (+ padding): glibc's resolver uses
// sendmmsg to send the A and AAAA queries together.
long sys_sendmmsg(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    u32 count = static_cast<u32>(min<u64>(a2, 1024));
    u32 sent = 0;
    for (; sent < count; sent++) {
        u64 entry = a1 + sent * 64ull;
        long r = send_one_msg(s.get(), s.nonblock(), entry, static_cast<int>(a3));
        if (r < 0)
            return sent ? sent : r;
        u32 len = static_cast<u32>(r);
        uaccess::put(reinterpret_cast<u32*>(entry + 56), len);
    }
    return sent;
}

long sys_recvmmsg(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    u32 count = static_cast<u32>(min<u64>(a2, 1024));
    u32 got = 0;
    for (; got < count; got++) {
        u64 entry = a1 + got * 64ull;
        // Only the first receive may block (MSG_WAITFORONE semantics).
        long r = recv_one_msg(s.get(), s.nonblock() || got > 0, entry, static_cast<int>(a3));
        if (r < 0)
            return got ? got : r;
        u32 len = static_cast<u32>(r);
        uaccess::put(reinterpret_cast<u32*>(entry + 56), len);
    }
    return got;
}

long sys_shutdown(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    if (a1 > 2)
        return -err::INVAL;
    return s->shutdown(static_cast<int>(a1));
}

constexpr int IPPROTO_IP = 0, IPPROTO_TCP = 6, IPPROTO_UDP = 17, IPPROTO_IPV6 = 41, IPPROTO_ICMPV6 = 58;
constexpr int IPV6_V6ONLY = 26;

long sys_setsockopt(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    int level = static_cast<int>(a1), name = static_cast<int>(a2);
    u8 raw[64] = {};
    u32 rlen = static_cast<u32>(min<u64>(a4, sizeof(raw)));
    if (a3 && rlen)
        if (long r = uaccess::copy_from(raw, reinterpret_cast<void*>(a3), rlen); r < 0)
            return r;
    if (long r = s->set_option(level, name, raw, rlen); r != OPT_UNHANDLED)
        return r;
    i32 value = 0;
    if (rlen >= 4)
        memcpy(&value, raw, 4);
    if (level == SOL_SOCKET) {
        switch (name) {
        case 2: s->reuse_addr = value; return 0;          // SO_REUSEADDR
        case 20: case 21: {                                // SO_RCVTIMEO / SO_SNDTIMEO (struct timeval)
            i64 tv[2] = {};
            memcpy(tv, raw, min<u32>(rlen, 16));
            u64 ns = static_cast<u64>(tv[0]) * 1'000'000'000ull + static_cast<u64>(tv[1]) * 1000;
            (name == 20 ? s->rcv_timeout_ns : s->snd_timeout_ns) = ns;
            return 0;
        }
        default: return 0; // SO_KEEPALIVE, SO_BROADCAST, buffer sizes, ...: accepted
        }
    }
    if (s->domain == AF_INET6 && level == IPPROTO_IPV6 && name == IPV6_V6ONLY) {
        if (s->bound)
            return -err::INVAL;
        s->v6only = value != 0;
        return 0;
    }
    bool inet = s->domain == AF_INET || s->domain == AF_INET6;
    // TCP_NODELAY (we never delay), IP_TOS, IP_RECVERR, IPV6_RECVHOPLIMIT, ICMP6_FILTER, ...: accepted
    if (inet && (level == IPPROTO_TCP || level == IPPROTO_IP || level == IPPROTO_UDP ||
                 (s->domain == AF_INET6 && (level == IPPROTO_IPV6 || level == IPPROTO_ICMPV6))))
        return 0;
    return -ENOPROTOOPT;
}

long sys_getsockopt(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    int level = static_cast<int>(a1), name = static_cast<int>(a2);
    u32 len;
    if (long r = uaccess::get(&len, reinterpret_cast<u32*>(a4)); r < 0)
        return r;
    u8 raw[64] = {};
    u32 rlen = sizeof(raw);
    long hr = s->get_option(level, name, raw, &rlen);
    if (hr != OPT_UNHANDLED) {
        if (hr < 0)
            return hr;
    } else {
        i32 value = 0;
        if (level == SOL_SOCKET) {
            switch (name) {
            case 3: value = s->type; break;                 // SO_TYPE
            case 4: value = s->error; s->error = 0; break;  // SO_ERROR
            case 2: value = s->reuse_addr; break;           // SO_REUSEADDR
            case 7: case 8: value = 212992; break;          // SO_SNDBUF / SO_RCVBUF
            case 38: value = s->protocol; break;            // SO_PROTOCOL
            case 39: value = s->domain; break;              // SO_DOMAIN
            default: value = 0; break;
            }
        } else if (level == IPPROTO_TCP && name == 2) {     // TCP_MAXSEG
            value = s->domain == AF_INET6 ? 1440 : 1460;
        } else if (level == IPPROTO_IPV6 && name == IPV6_V6ONLY) {
            value = s->v6only;
        }
        memcpy(raw, &value, 4);
        rlen = 4;
    }
    u32 n = min<u32>(len, rlen);
    if (long r = uaccess::copy_to(reinterpret_cast<void*>(a3), raw, n); r < 0)
        return r;
    return uaccess::put(reinterpret_cast<u32*>(a4), n);
}

} // namespace

void register_net() {
    set(41, sys_socket, "socket");
    set(42, sys_connect, "connect");
    set(43, sys_accept, "accept");
    set(44, sys_sendto, "sendto");
    set(45, sys_recvfrom, "recvfrom");
    set(46, sys_sendmsg, "sendmsg");
    set(47, sys_recvmsg, "recvmsg");
    set(48, sys_shutdown, "shutdown");
    set(49, sys_bind, "bind");
    set(50, sys_listen, "listen");
    set(51, sys_getsockname, "getsockname");
    set(52, sys_getpeername, "getpeername");
    set(53, sys_socketpair, "socketpair");
    set(54, sys_setsockopt, "setsockopt");
    set(55, sys_getsockopt, "getsockopt");
    set(288, sys_accept4, "accept4");
    set(299, sys_recvmmsg, "recvmmsg");
    set(307, sys_sendmmsg, "sendmmsg");
}

} // namespace syscall
