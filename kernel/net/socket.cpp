// BSD socket API (Linux ABI): socket, bind, connect, listen, accept,
// send/recv families, socket options, and the interface ioctls that
// ifconfig/route use.

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

Socket::Socket(int t, int p) : type(t), protocol(p) {
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

isize Socket::read_file(vfs::File* f, u64, void* buf, usize count) {
    usize n = min(count, CHUNK);
    u8* k = static_cast<u8*>(kmalloc(n ? n : 1));
    if (!k)
        return -err::NOMEM;
    usize full;
    isize r = recv(k, n, 0, nullptr, f->flags & vfs::O_NONBLOCK, &full);
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
        isize r = send(k, n, 0, nullptr, f->flags & vfs::O_NONBLOCK);
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

using net::Endpoint;
using net::SockAddrIn;
using net::Socket;
using namespace net;

constexpr long EAFNOSUPPORT = 97, EPROTONOSUPPORT = 93, ENOTSOCK = 88, EOPNOTSUPP = 95, ENOPROTOOPT = 92;
constexpr u32 SOCK_NONBLOCK = 04000, SOCK_CLOEXEC = 02000000;
constexpr usize CHUNK_SIZE = 64 * 1024;

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

long read_addr(u64 uaddr, u64 len, Endpoint* out, u16* family = nullptr) {
    if (len < 2)
        return -err::INVAL;
    SockAddrIn sa{};
    if (long r = uaccess::copy_from(&sa, reinterpret_cast<void*>(uaddr), min<u64>(len, sizeof(sa))); r < 0)
        return r;
    if (family)
        *family = sa.family;
    if (sa.family == AF_UNSPEC && family)
        return 0;
    if (sa.family != AF_INET)
        return -EAFNOSUPPORT;
    if (len < sizeof(sa))
        return -err::INVAL;
    out->addr = be32(sa.addr);
    out->port = be16(sa.port);
    return 0;
}

long write_addr(const Endpoint& ep, u64 uaddr, u64 ulen_ptr) {
    if (!uaddr || !ulen_ptr)
        return 0;
    u32 len;
    if (long r = uaccess::get(&len, reinterpret_cast<u32*>(ulen_ptr)); r < 0)
        return r;
    SockAddrIn sa{};
    sa.family = AF_INET;
    sa.port = be16(ep.port);
    sa.addr = be32(ep.addr);
    if (long r = uaccess::copy_to(reinterpret_cast<void*>(uaddr), &sa, min<u32>(len, sizeof(sa))); r < 0)
        return r;
    u32 out = sizeof(sa);
    return uaccess::put(reinterpret_cast<u32*>(ulen_ptr), out);
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
    if (domain != AF_INET)
        return -EAFNOSUPPORT; // IPv6, Unix and netlink sockets come later
    Socket* s = nullptr;
    if (type == SOCK_STREAM && (protocol == 0 || protocol == PROTO_TCP))
        s = create_tcp();
    else if (type == SOCK_DGRAM && (protocol == 0 || protocol == PROTO_UDP))
        s = create_udp(type, PROTO_UDP);
    else if ((type == SOCK_DGRAM || type == SOCK_RAW) && protocol == PROTO_ICMP)
        s = create_icmp(type);
    else
        return -EPROTONOSUPPORT;
    if (!s)
        return -err::NOMEM;
    return install(s, static_cast<u32>(a1));
}

long sys_socketpair(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return -EOPNOTSUPP; // needs AF_UNIX
}

long sys_bind(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    Endpoint ep;
    if (long r = read_addr(a1, a2, &ep); r < 0)
        return r;
    return s->bind(ep);
}

long sys_connect(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    Endpoint ep;
    u16 family = AF_INET;
    if (long r = read_addr(a1, a2, &ep, &family); r < 0)
        return r;
    if (family == AF_UNSPEC)
        ep = Endpoint{};
    return s->connect(ep, s.nonblock());
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
    long fd = install(child, static_cast<u32>(a3));
    if (fd < 0)
        return fd;
    write_addr(child->remote, a1, a2);
    return fd;
}

long sys_accept(SYSCALL_ARGS) { return sys_accept4(a0, a1, a2, 0, a4, a5, frame); }

long sys_getsockname(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    Endpoint ep = s->local;
    if (!ep.addr && s->connected) {
        LockGuard guard(g_lock);
        ep.addr = source_for(s->remote.addr);
    }
    return write_addr(ep, a1, a2);
}

long sys_getpeername(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    if (!s->connected)
        return -ENOTCONN;
    return write_addr(s->remote, a1, a2);
}

isize do_send(Socket* s, bool nonblock, const u8* user_buf, usize len, int flags, const Endpoint* to) {
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
        isize r = s->send(k, n, flags, to, nonblock || (flags & MSG_DONTWAIT));
        if (r < 0) {
            kfree(k);
            if (r == -err::PIPE && !(flags & MSG_NOSIGNAL))
                signal::send_thread(sched::current(), 13); // SIGPIPE
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
    Endpoint to;
    bool have_to = a4 != 0;
    if (have_to)
        if (long r = read_addr(a4, a5, &to); r < 0)
            return r;
    return do_send(s.get(), s.nonblock(), reinterpret_cast<const u8*>(a1), a2, static_cast<int>(a3),
                   have_to ? &to : nullptr);
}

isize do_recv(Socket* s, bool nonblock, u8* user_buf, usize len, int flags, Endpoint* from, usize* full) {
    usize n = min(len, CHUNK_SIZE);
    u8* k = static_cast<u8*>(kmalloc(n ? n : 1));
    if (!k)
        return -err::NOMEM;
    isize r = s->recv(k, n, flags, from, nonblock || (flags & MSG_DONTWAIT), full);
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
    Endpoint from = s->remote;
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

long send_one_msg(Socket* s, bool nonblock, u64 umsg, int flags) {
    MsgHdr m;
    if (long r = uaccess::get(&m, reinterpret_cast<MsgHdr*>(umsg)); r < 0)
        return r;
    Endpoint to;
    if (m.name && m.namelen)
        if (long r = read_addr(m.name, m.namelen, &to); r < 0)
            return r;
    u8* buf;
    usize len;
    if (long r = gather(m, &buf, &len); r < 0)
        return r;
    isize r = s->send(buf, len, flags, m.name && m.namelen ? &to : nullptr, nonblock || (flags & MSG_DONTWAIT));
    kfree(buf);
    if (r == -err::PIPE && !(flags & MSG_NOSIGNAL))
        signal::send_thread(sched::current(), 13);
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
    Endpoint from = s->remote;
    usize full = 0;
    isize r = s->recv(buf, total, flags, &from, nonblock || (flags & MSG_DONTWAIT), &full);
    if (r >= 0) {
        if (long e = scatter(m, buf, static_cast<usize>(r)); e < 0)
            r = e;
    }
    kfree(buf);
    if (r < 0)
        return r;
    MsgHdr out = m;
    out.controllen = 0; // no ancillary data
    out.flags = (s->type != SOCK_STREAM && full > static_cast<usize>(r)) ? MSG_TRUNC : 0;
    if (m.name && m.namelen >= sizeof(SockAddrIn)) {
        SockAddrIn sa{};
        sa.family = AF_INET;
        sa.port = be16(from.port);
        sa.addr = be32(from.addr);
        uaccess::copy_to(reinterpret_cast<void*>(m.name), &sa, sizeof(sa));
        out.namelen = sizeof(sa);
    } else {
        out.namelen = 0;
    }
    uaccess::put(reinterpret_cast<MsgHdr*>(umsg), out);
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

constexpr int SOL_SOCKET = 1, IPPROTO_IP = 0, IPPROTO_TCP = 6;

long sys_setsockopt(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    int level = static_cast<int>(a1), name = static_cast<int>(a2);
    i32 value = 0;
    if (a3 && a4 >= 4)
        if (long r = uaccess::get(&value, reinterpret_cast<i32*>(a3)); r < 0)
            return r;
    if (level == SOL_SOCKET) {
        switch (name) {
        case 2: s->reuse_addr = value; return 0;          // SO_REUSEADDR
        case 20: case 21: {                                // SO_RCVTIMEO / SO_SNDTIMEO (struct timeval)
            i64 tv[2] = {};
            if (long r = uaccess::copy_from(tv, reinterpret_cast<void*>(a3), min<u64>(a4, 16)); r < 0)
                return r;
            u64 ns = static_cast<u64>(tv[0]) * 1'000'000'000ull + static_cast<u64>(tv[1]) * 1000;
            (name == 20 ? s->rcv_timeout_ns : s->snd_timeout_ns) = ns;
            return 0;
        }
        default: return 0; // SO_KEEPALIVE, SO_BROADCAST, buffer sizes, ...: accepted
        }
    }
    if (level == IPPROTO_TCP || level == IPPROTO_IP)
        return 0; // TCP_NODELAY (we never delay), IP_TOS, IP_RECVERR, ...
    return -ENOPROTOOPT;
}

long sys_getsockopt(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    SockRef s(static_cast<long>(a0));
    if (long e = s.error()) return e;
    int level = static_cast<int>(a1), name = static_cast<int>(a2);
    i32 value = 0;
    if (level == SOL_SOCKET) {
        switch (name) {
        case 3: value = s->type; break;                 // SO_TYPE
        case 4: value = s->error; s->error = 0; break;  // SO_ERROR
        case 2: value = s->reuse_addr; break;           // SO_REUSEADDR
        case 7: case 8: value = 65536; break;           // SO_SNDBUF / SO_RCVBUF
        case 30: value = 1; break;                      // SO_ACCEPTCONN-ish answers are harmless
        case 38: value = s->protocol; break;            // SO_PROTOCOL
        case 39: value = AF_INET; break;                // SO_DOMAIN
        default: value = 0; break;
        }
    } else if (level == IPPROTO_TCP && name == 2) {     // TCP_MAXSEG
        value = 1460;
    }
    u32 len;
    if (long r = uaccess::get(&len, reinterpret_cast<u32*>(a4)); r < 0)
        return r;
    u32 n = min<u32>(len, 4);
    if (long r = uaccess::copy_to(reinterpret_cast<void*>(a3), &value, n); r < 0)
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
