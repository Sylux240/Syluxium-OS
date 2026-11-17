// UDP: datagram sockets (DNS, DHCP, games, streaming), over IPv4 and
// IPv6. AF_INET6 sockets also talk to IPv4 peers through v4-mapped
// addresses unless IPV6_V6ONLY is set. Datagrams bigger than the MTU are
// fragmented by the IP layer.

#include <mm/heap.hpp>
#include <net/internal.hpp>

namespace net {

namespace {

struct Datagram {
    Endpoint from;
    usize len;
    Datagram* next;
    u8 data[];
};

constexpr usize MAX_QUEUED_BYTES = 256 * 1024;
constexpr usize MAX_PAYLOAD4 = 65535 - 20 - 8;
constexpr usize MAX_PAYLOAD6 = 65535 - 8;

class UdpSocket;
UdpSocket* g_udp = nullptr; // all UDP sockets (lock held)

class UdpSocket : public Socket {
public:
    explicit UdpSocket(u16 d) : Socket(SOCK_DGRAM, PROTO_UDP, d) {
        LockGuard guard(g_lock);
        next = g_udp;
        g_udp = this;
    }

    UdpSocket* next = nullptr;
    Datagram* head = nullptr;
    Datagram* tail = nullptr;
    usize queued_bytes = 0;

    long bind(const Endpoint& ep) override {
        LockGuard guard(g_lock);
        return bind_locked(ep);
    }

    long bind_locked(const Endpoint& ep) {
        if (bound)
            return -err::INVAL;
        if (!ep.any() && !is_local_endpoint(ep) && !(!ep.v6 && ep.addr == 0xFFFFFFFF))
            return -EADDRNOTAVAIL;
        u16 port = ep.port;
        if (!port) {
            port = ephemeral_port(PROTO_UDP);
            if (!port)
                return -EADDRINUSE;
        } else if (port_in_use(PROTO_UDP, ep, port) && !reuse_addr) {
            return -EADDRINUSE;
        }
        local = ep;
        local.port = port;
        if (domain == AF_INET6 && ep.any() && !ep.v6)
            local.v6 = true; // implicit bind of an AF_INET6 socket: the dual-stack wildcard ::
        bound = true;
        return 0;
    }

    long connect(const Endpoint& ep, bool) override {
        LockGuard guard(g_lock);
        if (ep.any() && !ep.port) { // AF_UNSPEC: dissolve the association
            connected = false;
            remote = Endpoint{};
            return 0;
        }
        if (!bound)
            if (long r = bind_locked(Endpoint{}); r < 0)
                return r;
        remote = ep;
        connected = true;
        return 0;
    }

    isize send(const u8* data, usize len, int, const Endpoint* to, bool) override {
        LockGuard guard(g_lock);
        Endpoint dst;
        if (to)
            dst = *to;
        else if (connected)
            dst = remote;
        else
            return -EDESTADDRREQ;
        if (len > (dst.v6 ? MAX_PAYLOAD6 : MAX_PAYLOAD4))
            return -EMSGSIZE;
        if (!bound)
            if (long r = bind_locked(Endpoint{}); r < 0)
                return r;
        Endpoint src = local;
        if (src.any() || src.v6 != dst.v6) {
            if (!src.any())
                return -err::INVAL; // bound to an address of the other family
            bool broadcast = !dst.v6 && dst.addr == 0xFFFFFFFF;
            if (!source_endpoint(dst, &src) && !broadcast)
                return -ENETUNREACH;
        }
        u8* seg = static_cast<u8*>(kmalloc(8 + len));
        if (!seg)
            return -err::NOMEM;
        put16(seg, local.port);
        put16(seg + 2, dst.port);
        put16(seg + 4, static_cast<u16>(8 + len));
        put16(seg + 6, 0);
        memcpy(seg + 8, data, len);
        u16 sum = l4_checksum(src, dst, PROTO_UDP, seg, 8 + len);
        put16(seg + 6, sum ? sum : 0xFFFF);
        long r = ip_output(src, dst, PROTO_UDP, seg, 8 + len);
        kfree(seg);
        return r < 0 ? r : static_cast<isize>(len);
    }

    isize recv(u8* buf, usize len, int flags, Endpoint* from, bool nonblock, usize* full_len) override {
        for (;;) {
            if (long r = sock_wait(this, nonblock, rcv_timeout_ns, [&] { return head != nullptr || error; }); r < 0)
                return r;
            LockGuard guard(g_lock);
            if (error) {
                long e = error;
                error = 0;
                return -e;
            }
            Datagram* d = head;
            if (!d)
                continue;
            usize n = min(len, d->len);
            memcpy(buf, d->data, n);
            if (from)
                *from = d->from;
            if (full_len)
                *full_len = d->len;
            if (!(flags & MSG_PEEK)) {
                head = d->next;
                if (!head)
                    tail = nullptr;
                queued_bytes -= d->len;
                kfree(d);
            }
            return static_cast<isize>(n);
        }
    }

    long shutdown(int) override { return connected ? 0 : -ENOTCONN; }

    u16 poll_mask() override { return ::poll::POLLOUT | (head || error ? ::poll::POLLIN : 0) | (error ? ::poll::POLLERR : 0); }

    usize bytes_available() override { return head ? head->len : 0; }

    void deliver(const Endpoint& from, const u8* data, usize len) {
        if (queued_bytes + len > MAX_QUEUED_BYTES)
            return; // receiver too slow: drop, as UDP may
        auto* d = static_cast<Datagram*>(kmalloc(sizeof(Datagram) + len));
        if (!d)
            return;
        d->from = from;
        d->len = len;
        d->next = nullptr;
        memcpy(d->data, data, len);
        if (tail)
            tail->next = d;
        else
            head = d;
        tail = d;
        queued_bytes += len;
        wake();
    }

    void close() override {
        {
            LockGuard guard(g_lock);
            UdpSocket** link = &g_udp;
            while (*link && *link != this)
                link = &(*link)->next;
            if (*link)
                *link = next;
            while (head) {
                Datagram* d = head;
                head = d->next;
                kfree(d);
            }
        }
        delete this;
    }
};

void send_port_unreachable(const u8* ip) {
    // ICMP type 3 code 3, quoting the IP header + 8 bytes of the datagram.
    usize hl = (ip[0] & 0xF) * 4u;
    u8 msg[8 + 60 + 8] = {};
    msg[0] = 3;
    msg[1] = 3;
    memcpy(msg + 8, ip, hl + 8);
    put16(msg + 2, checksum_finish(checksum_add(0, msg, 8 + hl + 8)));
    ip_send(0, get32(ip + 12), PROTO_ICMP, msg, 8 + hl + 8);
}

} // namespace

Socket* create_udp(int, int, u16 domain) { return new UdpSocket(domain); }

void udp_input(Interface* in, const u8* ip, usize ip_len, const Endpoint& src_addr, const Endpoint& dst, const u8* data,
               usize len) {
    if (len < 8)
        return;
    u16 ulen = get16(data + 4);
    if (ulen < 8 || ulen > len)
        return;
    bool loop = in->flags & IFF_LOOPBACK;
    // IPv6 makes the checksum mandatory; IPv4 allows 0 = none.
    if ((get16(data + 6) || dst.v6) && !loop && l4_checksum(src_addr, dst, PROTO_UDP, data, ulen) != 0)
        return;
    u16 sport = get16(data), dport = get16(data + 2);
    if (dport == 68 && !dst.v6)
        dhcp_input(in, data + 8, ulen - 8u);

    Endpoint from = src_addr;
    from.port = sport;
    bool broadcast = !dst.v6 && (dst.addr == 0xFFFFFFFF || (in->netmask && dst.addr == (in->addr | ~in->netmask)));
    bool multicast = dst.v6 && ip6::is_multicast(dst.addr6);
    bool delivered = false;
    for (UdpSocket* s = g_udp; s; s = s->next) {
        if (!s->bound || s->local.port != dport)
            continue;
        if (broadcast || multicast) {
            if (dst.v6 ? s->domain != AF_INET6 : (s->domain == AF_INET6 && s->v6only))
                continue;
            if (!s->local.any())
                continue;
        } else if (!local_accepts(s, dst)) {
            continue;
        }
        if (s->connected && (!s->remote.same_addr(src_addr) || s->remote.port != sport))
            continue;
        s->deliver(from, data + 8, ulen - 8u);
        delivered = true;
        if (!broadcast && !multicast)
            break;
    }
    if (!delivered && !broadcast && !multicast && dport != 68) {
        if (dst.v6)
            icmp6_unreachable(ip, ip_len, 4); // port unreachable
        else
            send_port_unreachable(ip);
    }
}

// ICMP errors about our datagrams (e.g. port unreachable from a DNS server
// that isn't running): report ECONNREFUSED on connected sockets, like Linux.
void udp_icmp_error(u32 dst, u16 sport, u16 dport) {
    for (UdpSocket* s = g_udp; s; s = s->next)
        if (s->connected && s->local.port == sport && !s->remote.v6 && s->remote.addr == dst &&
            s->remote.port == dport) {
            s->error = static_cast<int>(ECONNREFUSED);
            s->wake();
        }
}

bool port_in_use_udp(const Endpoint& addr, u16 port) {
    for (UdpSocket* s = g_udp; s; s = s->next)
        if (s->bound && s->local.port == port && bindings_overlap(s->local, addr))
            return true;
    return false;
}

void for_each_udp(void (*fn)(Socket*, void*), void* ctx) {
    for (UdpSocket* s = g_udp; s; s = s->next)
        fn(s, ctx);
}

} // namespace net
