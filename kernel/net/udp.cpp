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
    Endpoint dst;  // the address it was sent to (IP_PKTINFO / IPV6_PKTINFO)
    u32 ifindex;   // the interface it arrived on
    u8 ttl;        // its TTL / hop limit (IP_TTL / IPV6_HOPLIMIT)
    usize len;
    Datagram* next;
    u8 data[];
};

// Socket options (Linux numbers).
constexpr int IPPROTO_IP_ = 0, IPPROTO_IPV6_ = 41;
constexpr int IP_TTL = 2, IP_PKTINFO = 8, IP_RECVTTL = 12, IP_MULTICAST_IF = 32, IP_MULTICAST_TTL = 33,
              IP_MULTICAST_LOOP = 34, IP_ADD_MEMBERSHIP = 35, IP_DROP_MEMBERSHIP = 36, IP_MULTICAST_ALL = 49;
constexpr int IPV6_UNICAST_HOPS = 16, IPV6_MULTICAST_IF = 17, IPV6_MULTICAST_HOPS = 18, IPV6_MULTICAST_LOOP = 19,
              IPV6_JOIN_GROUP = 20, IPV6_LEAVE_GROUP = 21, IPV6_RECVPKTINFO = 49, IPV6_PKTINFO = 50,
              IPV6_RECVHOPLIMIT = 51, IPV6_HOPLIMIT = 52;

i32 opt_int(const u8* val, u32 len) {
    if (len >= 4) {
        i32 v;
        memcpy(&v, val, 4);
        return v;
    }
    return len >= 1 ? val[0] : 0;
}

bool udp_deliver(Interface* in, const Endpoint& src_addr, const Endpoint& dst, const u8* data, usize ulen, u8 ttl);

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

    // Multicast and per-packet information (setsockopt; lock held).
    struct Member {
        Endpoint group;
        u32 ifindex;
    };
    static constexpr int MAX_MEMBERS = 20;
    Member members[MAX_MEMBERS];
    int nmembers = 0;
    u32 mc_ifindex = 0, mc_ifaddr = 0; // IP_MULTICAST_IF
    u32 mc_ifindex6 = 0;               // IPV6_MULTICAST_IF
    u8 mc_ttl = 1, mc_hops = 1;        // IP_MULTICAST_TTL / IPV6_MULTICAST_HOPS
    bool mc_loop = true, mc_loop6 = true;
    u8 ttl = 64, hops = 64;            // IP_TTL / IPV6_UNICAST_HOPS
    bool want_pktinfo = false, want_ttl = false, want_pktinfo6 = false, want_hoplimit6 = false;

    long membership(bool join, const Endpoint& group, u32 ifindex) {
        LockGuard guard(g_lock);
        if (!is_mcast(group))
            return -err::INVAL;
        if (join) {
            if (nmembers >= MAX_MEMBERS)
                return -ENOBUFS;
            Interface* f = mc_interface(group.v6, ifindex);
            if (!f)
                return -err::NODEV;
            for (int i = 0; i < nmembers; i++)
                if (members[i].ifindex == f->index && members[i].group.same_addr(group))
                    return -EADDRINUSE;
            long r = mc_join(f->index, group);
            if (r < 0)
                return r;
            members[nmembers++] = {group, static_cast<u32>(r)};
            return 0;
        }
        for (int i = 0; i < nmembers; i++) {
            if (members[i].group.same_addr(group) && (!ifindex || members[i].ifindex == ifindex)) {
                mc_leave(members[i].ifindex, group);
                members[i] = members[--nmembers];
                return 0;
            }
        }
        return -EADDRNOTAVAIL;
    }

    // IP_ADD_MEMBERSHIP takes struct ip_mreq {group, interface address} or
    // ip_mreqn {group, interface address, ifindex}.
    long ip_membership(bool join, const u8* val, u32 len) {
        if (len < 8)
            return -err::INVAL;
        u32 group, ifaddr, ifindex = 0;
        memcpy(&group, val, 4);
        memcpy(&ifaddr, val + 4, 4);
        if (len >= 12)
            memcpy(&ifindex, val + 8, 4);
        if (!ifindex && ifaddr) {
            LockGuard guard(g_lock);
            Interface* f = mc_interface(false, 0, be32(ifaddr));
            ifindex = f ? f->index : 0;
        }
        return membership(join, ep4(be32(group)), ifindex);
    }

    long set_option(int level, int name, const u8* val, u32 len) override {
        i32 v = opt_int(val, len);
        if (level == IPPROTO_IP_) {
            switch (name) {
            case IP_ADD_MEMBERSHIP: return ip_membership(true, val, len);
            case IP_DROP_MEMBERSHIP: return ip_membership(false, val, len);
            case IP_MULTICAST_IF: { // in_addr, ip_mreq or ip_mreqn
                u32 a = 0, idx = 0;
                if (len >= 4)
                    memcpy(&a, len >= 8 ? val + 4 : val, 4);
                if (len >= 12)
                    memcpy(&idx, val + 8, 4);
                mc_ifaddr = be32(a);
                mc_ifindex = idx;
                return 0;
            }
            case IP_MULTICAST_TTL:
                if (v < -1 || v > 255)
                    return -err::INVAL;
                mc_ttl = v < 0 ? 1 : static_cast<u8>(v);
                return 0;
            case IP_MULTICAST_LOOP: mc_loop = v != 0; return 0;
            case IP_TTL:
                if (v < -1 || v == 0 || v > 255)
                    return -err::INVAL;
                ttl = v < 0 ? 64 : static_cast<u8>(v);
                return 0;
            case IP_PKTINFO: want_pktinfo = v != 0; return 0;
            case IP_RECVTTL: want_ttl = v != 0; return 0;
            case IP_MULTICAST_ALL: return 0; // always: every member socket on the interface receives the group
            }
            return OPT_UNHANDLED;
        }
        if (level == IPPROTO_IPV6_) {
            switch (name) {
            case IPV6_JOIN_GROUP:
            case IPV6_LEAVE_GROUP: { // struct ipv6_mreq {in6_addr, ifindex}
                if (len < 20)
                    return -err::INVAL;
                u32 idx;
                memcpy(&idx, val + 16, 4);
                return membership(name == IPV6_JOIN_GROUP, ep6(val), idx);
            }
            case IPV6_MULTICAST_IF: mc_ifindex6 = static_cast<u32>(v); return 0;
            case IPV6_MULTICAST_HOPS:
                if (v < -1 || v > 255)
                    return -err::INVAL;
                mc_hops = v < 0 ? 1 : static_cast<u8>(v);
                return 0;
            case IPV6_UNICAST_HOPS:
                if (v < -1 || v > 255)
                    return -err::INVAL;
                hops = v < 0 ? 64 : static_cast<u8>(v);
                return 0;
            case IPV6_MULTICAST_LOOP: mc_loop6 = v != 0; return 0;
            case IPV6_RECVPKTINFO: want_pktinfo6 = v != 0; return 0;
            case IPV6_RECVHOPLIMIT: want_hoplimit6 = v != 0; return 0;
            }
            return OPT_UNHANDLED;
        }
        return OPT_UNHANDLED;
    }

    long get_option(int level, int name, u8* val, u32* len) override {
        i32 v;
        if (level == IPPROTO_IP_) {
            switch (name) {
            case IP_MULTICAST_TTL: v = mc_ttl; break;
            case IP_MULTICAST_LOOP: v = mc_loop; break;
            case IP_TTL: v = ttl; break;
            case IP_PKTINFO: v = want_pktinfo; break;
            case IP_RECVTTL: v = want_ttl; break;
            case IP_MULTICAST_ALL: v = 1; break;
            default: return OPT_UNHANDLED;
            }
        } else if (level == IPPROTO_IPV6_) {
            switch (name) {
            case IPV6_MULTICAST_IF: v = static_cast<i32>(mc_ifindex6); break;
            case IPV6_MULTICAST_HOPS: v = mc_hops; break;
            case IPV6_UNICAST_HOPS: v = hops; break;
            case IPV6_MULTICAST_LOOP: v = mc_loop6; break;
            case IPV6_RECVPKTINFO: v = want_pktinfo6; break;
            case IPV6_RECVHOPLIMIT: v = want_hoplimit6; break;
            default: return OPT_UNHANDLED;
            }
        } else {
            return OPT_UNHANDLED;
        }
        memcpy(val, &v, 4);
        *len = 4;
        return 0;
    }

    long bind(const Endpoint& ep) override {
        LockGuard guard(g_lock);
        return bind_locked(ep);
    }

    long bind_locked(const Endpoint& ep) {
        if (bound)
            return -err::INVAL;
        if (!ep.any() && !is_local_endpoint(ep) && !(!ep.v6 && ep.addr == 0xFFFFFFFF) && !is_mcast(ep))
            return -EADDRNOTAVAIL;
        u16 port = ep.port;
        if (!port) {
            port = ephemeral_port(PROTO_UDP);
            if (!port)
                return -EADDRINUSE;
        } else if (port_in_use(PROTO_UDP, ep, port, v6only) && !reuse_addr) {
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
        return send_ex(data, len, to, 0, nullptr);
    }

    isize send_msg(const u8* data, usize len, int, const SockAddr* to, Control* ctl, bool) override {
        Endpoint ep;
        if (to)
            if (long r = sockaddr_to_endpoint(*to, &ep, domain); r < 0)
                return r;
        bool pk = ctl && ctl->has_pktinfo;
        return send_ex(data, len, to ? &ep : nullptr, pk ? ctl->pkt_ifindex : 0,
                       pk && !ctl->pkt_src.any() ? &ctl->pkt_src : nullptr);
    }

    // Multicast out of one interface (IP_MULTICAST_IF / IP_PKTINFO's
    // ifindex), TTL 1 unless asked, and a copy for this host's own members
    // (IP_MULTICAST_LOOP). Lock held.
    isize send_multicast(const u8* data, usize len, const Endpoint& dst, u32 ifindex, const Endpoint* src_hint) {
        Interface* f = dst.v6 ? mc_interface(true, ifindex ? ifindex : (dst.scope ? dst.scope : mc_ifindex6))
                              : mc_interface(false, ifindex ? ifindex : mc_ifindex, ifindex ? 0 : mc_ifaddr);
        if (!f)
            return -ENETUNREACH;
        Endpoint src = local;
        if (src_hint && src_hint->v6 == dst.v6)
            src = *src_hint;
        if (src.any() || src.v6 != dst.v6 || is_mcast(src)) {
            u16 port = src.port;
            src = Endpoint{};
            src.v6 = dst.v6;
            src.port = port;
            if (dst.v6) {
                src.scope = f->index;
                if (!source6_for(dst.addr6, f->index, src.addr6))
                    return -EADDRNOTAVAIL;
            } else {
                src.addr = f->addr;
            }
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
        u8 t = dst.v6 ? mc_hops : mc_ttl;
        long r = dst.v6 ? ip6_send(src.addr6, dst.addr6, f->index, PROTO_UDP, seg, 8 + len, t)
                        : ip_send(src.addr, dst.addr, PROTO_UDP, seg, 8 + len, t, f);
        if (r >= 0 && (dst.v6 ? mc_loop6 : mc_loop) && mc_joined(f->index, dst)) {
            Endpoint lsrc = src, ldst = dst;
            lsrc.scope = ldst.scope = dst.v6 ? f->index : 0;
            udp_deliver(f, lsrc, ldst, seg, 8 + len, t);
        }
        kfree(seg);
        return r < 0 ? r : static_cast<isize>(len);
    }

    isize send_ex(const u8* data, usize len, const Endpoint* to, u32 ifindex, const Endpoint* src_hint) {
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
        if (is_mcast(dst))
            return send_multicast(data, len, dst, ifindex, src_hint);
        Endpoint src = local;
        if (src.any() || src.v6 != dst.v6 || is_mcast(src)) {
            if (is_mcast(src))
                src = Endpoint{};
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
        return take(buf, len, flags, from, nonblock, full_len, nullptr);
    }

    isize recv_msg(u8* buf, usize len, int flags, SockAddr* from, Control* ctl, bool nonblock,
                   usize* full_len) override {
        Endpoint ep = remote;
        Datagram meta{};
        isize r = take(buf, len, flags, &ep, nonblock, full_len, &meta);
        if (r < 0)
            return r;
        if (from)
            endpoint_to_sockaddr(ep, from, domain);
        if (ctl)
            add_packet_info(*ctl, meta);
        return r;
    }

    // IP_PKTINFO {ifindex, local address, header destination}, IP_TTL,
    // IPV6_PKTINFO {address, ifindex}, IPV6_HOPLIMIT, as asked for.
    void add_packet_info(Control& ctl, const Datagram& m) {
        i32 t = m.ttl;
        if (m.dst.v6) {
            if (want_pktinfo6) {
                u8 pi[20];
                memcpy(pi, m.dst.addr6, 16);
                memcpy(pi + 16, &m.ifindex, 4);
                control_add_ip(ctl, IPPROTO_IPV6_, IPV6_PKTINFO, pi, 20);
            }
            if (want_hoplimit6)
                control_add_ip(ctl, IPPROTO_IPV6_, IPV6_HOPLIMIT, &t, 4);
            return;
        }
        if (want_pktinfo) {
            u8 pi[12];
            u32 local_addr = m.dst.addr;
            {
                LockGuard guard(g_lock);
                Interface* f = interface_by_index(m.ifindex);
                if (f && f->addr)
                    local_addr = f->addr;
            }
            u32 spec = be32(local_addr), addr = be32(m.dst.addr);
            memcpy(pi, &m.ifindex, 4);
            memcpy(pi + 4, &spec, 4);
            memcpy(pi + 8, &addr, 4);
            control_add_ip(ctl, IPPROTO_IP_, IP_PKTINFO, pi, 12);
        }
        if (want_ttl)
            control_add_ip(ctl, IPPROTO_IP_, IP_TTL, &t, 4);
    }

    isize take(u8* buf, usize len, int flags, Endpoint* from, bool nonblock, usize* full_len, Datagram* meta) {
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
            if (meta) {
                meta->dst = d->dst;
                meta->ifindex = d->ifindex;
                meta->ttl = d->ttl;
            }
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

    void deliver(const Endpoint& from, const Endpoint& to, u32 ifindex, u8 t, const u8* data, usize len) {
        if (queued_bytes + len > MAX_QUEUED_BYTES)
            return; // receiver too slow: drop, as UDP may
        auto* d = static_cast<Datagram*>(kmalloc(sizeof(Datagram) + len));
        if (!d)
            return;
        d->from = from;
        d->dst = to;
        d->ifindex = ifindex;
        d->ttl = t;
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
            for (int i = 0; i < nmembers; i++)
                mc_leave(members[i].ifindex, members[i].group);
            nmembers = 0;
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

// Hand a checked UDP datagram (header included) to the sockets that want
// it; broadcasts and multicasts to every one of them. False if no socket
// took a unicast datagram (the caller then reports the port unreachable).
bool udp_deliver(Interface* in, const Endpoint& src_addr, const Endpoint& dst, const u8* data, usize ulen, u8 ttl) {
    u16 sport = get16(data), dport = get16(data + 2);
    Endpoint from = src_addr;
    from.port = sport;
    bool broadcast = !dst.v6 && (dst.addr == 0xFFFFFFFF || (in->netmask && dst.addr == (in->addr | ~in->netmask)));
    bool multicast = is_mcast(dst);
    if (multicast && !mc_joined(in->index, dst))
        return true; // nobody here joined that group
    bool delivered = false;
    for (UdpSocket* s = g_udp; s; s = s->next) {
        if (!s->bound || s->local.port != dport)
            continue;
        if (broadcast || multicast) {
            if (dst.v6 ? s->domain != AF_INET6 : (s->domain == AF_INET6 && s->v6only))
                continue;
            if (!s->local.any() && !(multicast && s->local.same_addr(dst)))
                continue;
        } else if (!local_accepts(s, dst)) {
            continue;
        }
        if (s->connected && (!s->remote.same_addr(src_addr) || s->remote.port != sport))
            continue;
        s->deliver(from, dst, in->index, ttl, data + 8, ulen - 8u);
        delivered = true;
        if (!broadcast && !multicast)
            break;
    }
    return delivered || broadcast || multicast;
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
    u16 dport = get16(data + 2);
    if (dport == 68 && !dst.v6)
        dhcp_input(in, data + 8, ulen - 8u);
    u8 ttl = ip ? (dst.v6 ? ip[7] : ip[8]) : 64;
    if (!udp_deliver(in, src_addr, dst, data, ulen, ttl) && dport != 68 && ip) {
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

bool port_in_use_udp(const Endpoint& addr, u16 port, bool v6only) {
    for (UdpSocket* s = g_udp; s; s = s->next)
        if (s->bound && s->local.port == port && bindings_overlap(s->local, addr, s->v6only, v6only))
            return true;
    return false;
}

void for_each_udp(void (*fn)(Socket*, void*), void* ctx) {
    for (UdpSocket* s = g_udp; s; s = s->next)
        fn(s, ctx);
}

} // namespace net
