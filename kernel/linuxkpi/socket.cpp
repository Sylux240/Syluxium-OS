// LinuxKPI sockets: socket(2) for address families implemented by Linux
// code (AF_BLUETOOTH, ...). A LinuxSocket is a net::Socket whose
// operations call the family's proto_ops on a struct socket, through the
// C side in glue/sock.c (lkpi_socket_*). Data crosses as kernel buffers (the
// socket layer has already copied it in from the program); blocking
// happens inside the Linux code, on the sock's wait queue, so non-blocking
// calls ask for MSG_DONTWAIT / O_NONBLOCK there.

#include <fs/poll.hpp>
#include <lib/errno.hpp>
#include <lib/string.hpp>
#include <linuxkpi/linuxkpi.hpp>
#include <net/internal.hpp>

extern "C" {
void* lkpi_socket_create(int family, int type, int protocol, long* err);
void lkpi_socket_set_native(void* s, void* native);
void* lkpi_socket_get_native(void* s);
long lkpi_socket_bind(void* s, const void* addr, int len);
long lkpi_socket_connect(void* s, const void* addr, int len, int nonblock);
long lkpi_socket_listen(void* s, int backlog);
long lkpi_socket_accept(void* s, void** out, int nonblock);
long lkpi_socket_sendmsg(void* s, const void* data, usize len, const void* name, int namelen, int flags);
long lkpi_socket_recvmsg(void* s, void* buf, usize len, void* name, int* namelen, int flags);
long lkpi_socket_getname(void* s, void* addr, int peer);
long lkpi_socket_shutdown(void* s, int how);
unsigned lkpi_socket_poll(void* s);
long lkpi_socket_setsockopt(void* s, int level, int name, const void* val, unsigned len);
long lkpi_socket_getsockopt(void* s, int level, int name, void* val, int* len);
long lkpi_socket_ioctl(void* s, unsigned cmd, unsigned long arg);
void lkpi_socket_release(void* s);
}

namespace linuxkpi {

namespace {

constexpr int SOL_SOCKET = 1, MSG_DONTWAIT = 0x40;
constexpr long ERESTARTSYS = 512, ERESTARTNOINTR = 513, ERESTARTNOHAND = 514, ENOIOCTLCMD = 515;
// Linux poll bits (EPOLL*).
constexpr unsigned EP_IN = 0x1, EP_PRI = 0x2, EP_OUT = 0x4, EP_ERR = 0x8, EP_HUP = 0x10, EP_RDNORM = 0x40,
                   EP_WRNORM = 0x100, EP_RDHUP = 0x2000;

// Linux's "restart the system call" codes reach programs as EINTR here.
long errno_of(long r) {
    if (r == -ERESTARTSYS || r == -ERESTARTNOINTR || r == -ERESTARTNOHAND)
        return -err::INTR;
    if (r == -ENOIOCTLCMD)
        return -25; // ENOTTY
    return r;
}

class LinuxSocket final : public net::Socket {
public:
    LinuxSocket(void* s, int type, int protocol, u16 family) : Socket(type, protocol, family), m_sock(s) {
        lkpi_socket_set_native(s, this);
    }

    long bind_addr(const net::SockAddr& sa) override {
        return errno_of(lkpi_socket_bind(m_sock, sa.data, static_cast<int>(sa.len)));
    }

    long connect_addr(const net::SockAddr& sa, bool nonblock) override {
        long r = lkpi_socket_connect(m_sock, sa.data, static_cast<int>(sa.len), nonblock);
        if (r == 0)
            connected = true;
        return errno_of(r);
    }

    long listen(int backlog) override { return errno_of(lkpi_socket_listen(m_sock, backlog)); }

    long accept(net::Socket** out, bool nonblock) override {
        void* n = nullptr;
        long r = lkpi_socket_accept(m_sock, &n, nonblock);
        if (r < 0)
            return errno_of(r);
        auto* child = new LinuxSocket(n, type, protocol, domain);
        child->connected = true;
        *out = child;
        return 0;
    }

    isize send_msg(const u8* data, usize len, int flags, const net::SockAddr* to, net::Control*,
                   bool nonblock) override {
        return errno_of(lkpi_socket_sendmsg(m_sock, data, len, to && to->len ? to->data : nullptr,
                                            to ? static_cast<int>(to->len) : 0,
                                            flags | (nonblock ? MSG_DONTWAIT : 0)));
    }

    isize recv_msg(u8* buf, usize len, int flags, net::SockAddr* from, net::Control*, bool nonblock,
                   usize* full_len) override {
        u8 name[128] = {};
        int namelen = 0;
        long r = lkpi_socket_recvmsg(m_sock, buf, len, name, &namelen, flags | (nonblock ? MSG_DONTWAIT : 0));
        if (r < 0)
            return errno_of(r);
        if (from) {
            *from = net::SockAddr{};
            from->len = namelen > 0 && namelen <= 128 ? static_cast<u32>(namelen) : 0;
            memcpy(from->data, name, from->len);
        }
        if (full_len)
            *full_len = static_cast<usize>(r);
        return r < static_cast<long>(len) ? r : static_cast<long>(len);
    }

    isize send(const u8* data, usize len, int flags, const net::Endpoint*, bool nonblock) override {
        return send_msg(data, len, flags, nullptr, nullptr, nonblock);
    }
    isize recv(u8* buf, usize len, int flags, net::Endpoint*, bool nonblock, usize* full_len) override {
        return recv_msg(buf, len, flags, nullptr, nullptr, nonblock, full_len);
    }

    long get_name(net::SockAddr* out, bool peer) override {
        *out = net::SockAddr{};
        long r = lkpi_socket_getname(m_sock, out->data, peer ? 1 : 0);
        if (r < 0)
            return errno_of(r);
        out->len = static_cast<u32>(r);
        return 0;
    }

    long shutdown(int how) override { return errno_of(lkpi_socket_shutdown(m_sock, how)); }

    u16 poll_mask() override {
        unsigned m = lkpi_socket_poll(m_sock);
        u16 out = 0;
        if (m & (EP_IN | EP_RDNORM))
            out |= ::poll::POLLIN | ::poll::POLLRDNORM;
        if (m & EP_PRI)
            out |= ::poll::POLLPRI;
        if (m & (EP_OUT | EP_WRNORM))
            out |= ::poll::POLLOUT | ::poll::POLLWRNORM;
        if (m & EP_ERR)
            out |= ::poll::POLLERR;
        if (m & (EP_HUP | EP_RDHUP))
            out |= ::poll::POLLHUP;
        return out;
    }

    // SOL_SOCKET stays with the generic code; the family's own levels go to it.
    long set_option(int level, int name, const u8* val, u32 len) override {
        if (level == SOL_SOCKET)
            return net::OPT_UNHANDLED;
        return errno_of(lkpi_socket_setsockopt(m_sock, level, name, val, len));
    }

    long get_option(int level, int name, u8* val, u32* len) override {
        if (level == SOL_SOCKET)
            return net::OPT_UNHANDLED;
        int ilen = static_cast<int>(*len);
        long r = lkpi_socket_getsockopt(m_sock, level, name, val, &ilen);
        if (r < 0)
            return errno_of(r);
        *len = static_cast<u32>(ilen);
        return 0;
    }

    long ioctl(u64 request, u64 arg) override {
        long r = lkpi_socket_ioctl(m_sock, static_cast<unsigned>(request), arg);
        if (r != -ENOIOCTLCMD)
            return errno_of(r);
        return Socket::ioctl(request, arg);
    }

    void close() override {
        lkpi_socket_set_native(m_sock, nullptr);
        lkpi_socket_release(m_sock); // the family's release, then the struct socket
        delete this;
    }

private:
    void* m_sock;
};

} // namespace

net::Socket* create_socket(int family, int type, int protocol, long* error) {
    void* s = lkpi_socket_create(family, type, protocol, error);
    if (!s) {
        *error = errno_of(*error);
        return nullptr;
    }
    return new LinuxSocket(s, type, protocol, static_cast<u16>(family));
}

} // namespace linuxkpi

extern "C" void lkpi_sock_notify_native(void* native) {
    if (native)
        static_cast<net::Socket*>(native)->wake();
    else
        poll::notify();
}
