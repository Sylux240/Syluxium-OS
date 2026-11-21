// AF_NETLINK sockets. NETLINK_ROUTE ("rtnetlink") answers the requests
// that `ip addr/link/route` (busybox, iproute2) and glibc's getifaddrs()
// and getaddrinfo(AI_ADDRCONFIG) send: link, address and route dumps, and
// adding/removing addresses, routes and link flags. NETLINK_KOBJECT_UEVENT
// sockets receive device add/remove events (uevent_broadcast()): the
// kernel's format on group 1 and, since there is no udevd, udevd's own
// format on group 2, where libudev monitors (libinput, wlroots) listen.
// Other protocols are refused when the socket is created.
//
// Requests are handled synchronously inside sendmsg(): the replies are
// queued on the sender's socket as datagrams of at most one page (glibc
// reads with a page-sized buffer and fails on MSG_TRUNC).

#include <mm/heap.hpp>
#include <net/internal.hpp>
#include <lib/format.hpp>
#include <net/uevent.hpp>
#include <time/clock.hpp>
#include <proc/process.hpp>

extern "C" {
void lkpi_genl_rcv(u32 portid, const void* data, usize len);
void lkpi_netlink_release(int protocol, u32 portid);
}

namespace net {

namespace {

constexpr int NETLINK_ROUTE = 0;
constexpr int NETLINK_GENERIC = 16;
constexpr u16 NLMSG_ERROR = 2, NLMSG_DONE = 3;
constexpr u16 NLM_F_REQUEST = 0x1, NLM_F_MULTI = 0x2, NLM_F_ACK = 0x4, NLM_F_DUMP = 0x300;
constexpr u16 RTM_NEWLINK = 16, RTM_DELLINK = 17, RTM_GETLINK = 18, RTM_SETLINK = 19, RTM_NEWADDR = 20,
              RTM_DELADDR = 21, RTM_GETADDR = 22, RTM_NEWROUTE = 24, RTM_DELROUTE = 25, RTM_GETROUTE = 26;
constexpr u16 IFLA_ADDRESS = 1, IFLA_BROADCAST = 2, IFLA_IFNAME = 3, IFLA_MTU = 4, IFLA_QDISC = 6, IFLA_STATS = 7,
              IFLA_TXQLEN = 13, IFLA_OPERSTATE = 16, IFLA_LINKMODE = 17, IFLA_STATS64 = 23, IFLA_GROUP = 27;
constexpr u16 IFA_ADDRESS = 1, IFA_LOCAL = 2, IFA_LABEL = 3, IFA_BROADCAST = 4, IFA_FLAGS = 8;
constexpr u16 RTA_DST = 1, RTA_OIF = 4, RTA_GATEWAY = 5, RTA_PRIORITY = 6, RTA_PREFSRC = 7, RTA_TABLE = 15;
constexpr u32 IFF_LOWER_UP = 0x10000;
constexpr u8 RT_TABLE_MAIN = 254, RTPROT_KERNEL = 2, RTPROT_DHCP = 16, RT_SCOPE_UNIVERSE = 0,
             RT_SCOPE_LINK = 253, RT_SCOPE_HOST = 254, RTN_UNICAST = 1;
constexpr u8 IFA_F_PERMANENT = 0x80;
constexpr usize DGRAM_LIMIT = 4096;
constexpr usize QUEUE_LIMIT = 1024 * 1024;

struct NlMsgHdr {
    u32 len;
    u16 type;
    u16 flags;
    u32 seq;
    u32 pid;
};
static_assert(sizeof(NlMsgHdr) == 16);

constexpr usize align4(usize n) { return (n + 3) & ~usize(3); }

struct Datagram {
    Datagram* next;
    usize len;
    u32 src_group; // multicast group it was sent to (the sender address's nl_groups)
    bool cred;     // comes with SCM_CREDENTIALS (uid 0): libudev drops uevents without
    u8 data[];
};

class NetlinkSocket;
NetlinkSocket* g_sockets = nullptr; // for port id allocation (g_nl held)
sched::Mutex g_nl;
i32 g_next_auto_port = -4096;

// ---- reply construction ----

// Builds netlink messages and packs them into page-sized datagrams.
class Builder {
public:
    Builder(u32 seq, u32 portid) : m_seq(seq), m_pid(portid) {}
    ~Builder() {
        while (m_head) {
            Datagram* d = m_head;
            m_head = d->next;
            kfree(d);
        }
    }

    // Start a message; attributes follow with attr().
    void begin(u16 type, u16 flags, const void* fixed, usize fixed_len) {
        m_len = sizeof(NlMsgHdr);
        NlMsgHdr h{0, type, flags, m_seq, m_pid};
        memcpy(m_msg, &h, sizeof(h));
        append(fixed, fixed_len);
    }

    void attr(u16 type, const void* data, usize len) {
        u16 hdr[2] = {static_cast<u16>(4 + len), type};
        append(hdr, 4);
        append(data, len);
    }
    void attr_u32(u16 type, u32 v) { attr(type, &v, 4); }
    void attr_u8(u16 type, u8 v) { attr(type, &v, 1); }
    void attr_str(u16 type, const char* s) { attr(type, s, strlen(s) + 1); }
    void attr_be32(u16 type, u32 host_order) {
        u32 v = be32(host_order);
        attr(type, &v, 4);
    }

    // Finish the message and add it to the current datagram.
    void end() {
        if (m_overflow) {
            m_overflow = false;
            return;
        }
        u32 len = static_cast<u32>(m_len);
        memcpy(m_msg, &len, 4);
        if (m_cur_len + m_len > DGRAM_LIMIT)
            flush();
        memcpy(m_cur + m_cur_len, m_msg, m_len);
        m_cur_len += align4(m_len);
    }

    void done(u16 flags) {
        i32 zero = 0;
        begin(NLMSG_DONE, flags, &zero, 4);
        end();
    }

    void error(i32 err, const NlMsgHdr& req) {
        u8 payload[4 + sizeof(NlMsgHdr)];
        memcpy(payload, &err, 4);
        memcpy(payload + 4, &req, sizeof(req));
        begin(NLMSG_ERROR, 0, payload, sizeof(payload));
        end();
    }

    // Close the current datagram.
    void flush() {
        if (!m_cur_len)
            return;
        auto* d = static_cast<Datagram*>(kmalloc(sizeof(Datagram) + m_cur_len));
        if (d) {
            d->next = nullptr;
            d->src_group = 0;
            d->cred = false;
            d->len = m_cur_len;
            memcpy(d->data, m_cur, m_cur_len);
            *m_tail = d;
            m_tail = &d->next;
        }
        m_cur_len = 0;
    }

    // Take the finished datagrams.
    Datagram* take() {
        flush();
        Datagram* d = m_head;
        m_head = nullptr;
        m_tail = &m_head;
        return d;
    }

private:
    void append(const void* data, usize len) {
        usize padded = align4(len);
        if (m_len + padded > sizeof(m_msg)) {
            m_overflow = true;
            return;
        }
        memcpy(m_msg + m_len, data, len);
        memset(m_msg + m_len + len, 0, padded - len);
        m_len += padded;
    }

    u32 m_seq, m_pid;
    u8 m_msg[1024];
    usize m_len = 0;
    bool m_overflow = false;
    u8 m_cur[DGRAM_LIMIT];
    usize m_cur_len = 0;
    Datagram* m_head = nullptr;
    Datagram** m_tail = &m_head;
};

// IPv6 parts (defined further down).
long change_addr6(bool add, const u8* p, usize len);
long change_route6(bool add, const u8* p, usize len);
void dump_addr6(Builder& b);
void dump_routes6(Builder& b);

u8 prefix_len(u32 mask) { return static_cast<u8>(__builtin_popcount(mask)); }
u32 mask_of(u8 prefix) { return prefix == 0 ? 0 : prefix >= 32 ? 0xFFFFFFFF : ~((1u << (32 - prefix)) - 1); }

// ---- RTM_*LINK ----

void put_link(Builder& b, Interface* f, u16 flags) {
    bool lo = f->flags & IFF_LOOPBACK;
    struct {
        u8 family, pad;
        u16 type;
        i32 index;
        u32 flags, change;
    } ifi{AF_UNSPEC, 0, static_cast<u16>(lo ? 772 : 1), static_cast<i32>(f->index), 0, 0xFFFFFFFF};
    ifi.flags = f->flags & ~IFF_RUNNING;
    if ((f->flags & IFF_UP) && f->link_up)
        ifi.flags |= IFF_RUNNING | IFF_LOWER_UP;
    b.begin(RTM_NEWLINK, flags, &ifi, sizeof(ifi));
    b.attr_str(IFLA_IFNAME, f->name);
    b.attr_u32(IFLA_TXQLEN, lo ? 1000 : 1000);
    b.attr_u8(IFLA_OPERSTATE, lo ? 0 : ((f->flags & IFF_UP) && f->link_up) ? 6 : 2); // UNKNOWN / UP / DOWN
    b.attr_u8(IFLA_LINKMODE, 0);
    b.attr_u32(IFLA_MTU, f->mtu);
    b.attr_u32(IFLA_GROUP, 0);
    b.attr_str(IFLA_QDISC, lo ? "noqueue" : "pfifo_fast");
    static const u8 bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static const u8 zero[6] = {};
    b.attr(IFLA_ADDRESS, lo ? zero : f->mac, 6);
    b.attr(IFLA_BROADCAST, lo ? zero : bcast, 6);
    // rtnl_link_stats (32-bit) and rtnl_link_stats64: same field order.
    u64 s64[24] = {f->rx_packets, f->tx_packets, f->rx_bytes, f->tx_bytes, 0, f->tx_errors, f->rx_dropped, 0};
    u32 s32[24];
    for (int i = 0; i < 24; i++)
        s32[i] = static_cast<u32>(s64[i]);
    b.attr(IFLA_STATS, s32, sizeof(s32));
    b.attr(IFLA_STATS64, s64, sizeof(s64));
    b.end();
}

// Find the interface a link/address request names (index, else IFLA_IFNAME).
Interface* find_link(i32 index, const u8* attrs, usize len) {
    if (index > 0)
        return interface_by_index(static_cast<u32>(index));
    for (usize pos = 0; pos + 4 <= len;) {
        u16 alen = static_cast<u16>(attrs[pos] | (attrs[pos + 1] << 8));
        u16 atype = static_cast<u16>(attrs[pos + 2] | (attrs[pos + 3] << 8)) & 0x3FFF;
        if (alen < 4 || pos + alen > len)
            break;
        if (atype == IFLA_IFNAME) {
            char name[16] = {};
            memcpy(name, attrs + pos + 4, min<usize>(alen - 4u, 15));
            return interface_by_name(name);
        }
        pos += align4(alen);
    }
    return nullptr;
}

// Call fn(type, data, len) for every attribute.
template <typename Fn>
void for_each_attr(const u8* attrs, usize len, Fn fn) {
    for (usize pos = 0; pos + 4 <= len;) {
        u16 alen = static_cast<u16>(attrs[pos] | (attrs[pos + 1] << 8));
        u16 atype = static_cast<u16>(attrs[pos + 2] | (attrs[pos + 3] << 8)) & 0x3FFF;
        if (alen < 4 || pos + alen > len)
            break;
        fn(atype, attrs + pos + 4, static_cast<usize>(alen - 4));
        pos += align4(alen);
    }
}

long set_link(const u8* p, usize len) {
    if (len < 16)
        return -err::INVAL;
    i32 index;
    u32 flags, change;
    memcpy(&index, p + 4, 4);
    memcpy(&flags, p + 8, 4);
    memcpy(&change, p + 12, 4);
    Interface* f = find_link(index, p + 16, len - 16);
    if (!f)
        return -err::NODEV;
    // As in Linux: flags change only if ifi_flags or ifi_change is set; a
    // zero ifi_change (old tools) means "all flags". Requests that only set
    // attributes (wpa_supplicant/hostapd's IFLA_OPERSTATE) send both as 0.
    long result = 0;
    if (flags || change) {
        if (change == 0)
            change = 0xFFFFFFFF;
        if ((change & IFF_UP) && ((flags ^ f->flags) & IFF_UP))
            result = f->set_up(flags & IFF_UP);
    }
    for_each_attr(p + 16, len - 16, [&](u16 type, const u8* data, usize alen) {
        if (type == IFLA_MTU && alen >= 4) {
            u32 mtu;
            memcpy(&mtu, data, 4);
            if (mtu < 68)
                result = -err::INVAL;
            else
                f->mtu = mtu;
        }
    });
    return result;
}

// ---- RTM_*ADDR ----

void put_addr4(Builder& b, Interface* f, u16 flags) {
    bool lo = f->flags & IFF_LOOPBACK;
    struct {
        u8 family, prefixlen, flags, scope;
        u32 index;
    } ifa{AF_INET, prefix_len(f->netmask), IFA_F_PERMANENT, lo ? RT_SCOPE_HOST : RT_SCOPE_UNIVERSE, f->index};
    b.begin(RTM_NEWADDR, flags, &ifa, sizeof(ifa));
    b.attr_be32(IFA_ADDRESS, f->addr);
    b.attr_be32(IFA_LOCAL, f->addr);
    if (!lo)
        b.attr_be32(IFA_BROADCAST, f->addr | ~f->netmask);
    b.attr_str(IFA_LABEL, f->name);
    b.attr_u32(IFA_FLAGS, IFA_F_PERMANENT);
    b.end();
}

long change_addr(bool add, const u8* p, usize len) {
    if (len < 8)
        return -err::INVAL;
    u8 family = p[0], prefix = p[1];
    u32 index;
    memcpy(&index, p + 4, 4);
    if (family != AF_INET)
        return family == AF_INET6 ? change_addr6(add, p, len) : -EAFNOSUPPORT;
    Interface* f = interface_by_index(index);
    if (!f)
        return -err::NODEV;
    u32 addr = 0;
    for_each_attr(p + 8, len - 8, [&](u16 type, const u8* data, usize alen) {
        if ((type == IFA_LOCAL || (type == IFA_ADDRESS && !addr)) && alen >= 4) {
            u32 v;
            memcpy(&v, data, 4);
            addr = be32(v);
        }
    });
    if (!addr)
        return -err::INVAL;
    if (add) {
        if (f->addr == addr && f->netmask == mask_of(prefix))
            return -err::EXIST;
        f->addr = addr; // one IPv4 address per interface
        f->netmask = mask_of(prefix);
    } else {
        if (f->addr != addr)
            return -EADDRNOTAVAIL;
        f->addr = 0;
        f->netmask = 0;
    }
    return 0;
}

// ---- RTM_*ROUTE ----

struct RtMsg {
    u8 family, dst_len, src_len, tos, table, protocol, scope, type;
    u32 flags;
};

void put_route4(Builder& b, u16 flags, Interface* f, u32 dst, u8 dst_len, u32 gw) {
    RtMsg rt{AF_INET, dst_len, 0, 0, RT_TABLE_MAIN, gw ? RTPROT_DHCP : RTPROT_KERNEL,
             gw ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK, RTN_UNICAST, 0};
    b.begin(RTM_NEWROUTE, flags, &rt, sizeof(rt));
    b.attr_u32(RTA_TABLE, RT_TABLE_MAIN);
    if (dst_len)
        b.attr_be32(RTA_DST, dst);
    if (gw) {
        b.attr_be32(RTA_GATEWAY, gw);
        b.attr_u32(RTA_PRIORITY, 100);
    } else {
        b.attr_be32(RTA_PREFSRC, f->addr);
    }
    b.attr_u32(RTA_OIF, f->index);
    b.end();
}

void dump_routes4(Builder& b, u16 flags) {
    u32 gw = gateway();
    if (gw) {
        for (u32 i = 1; Interface* f = interface_by_index(i); i++)
            if (!(f->flags & IFF_LOOPBACK) && f->addr && (gw & f->netmask) == (f->addr & f->netmask)) {
                put_route4(b, flags, f, 0, 0, gw);
                break;
            }
    }
    for (u32 i = 1; Interface* f = interface_by_index(i); i++)
        if (!(f->flags & IFF_LOOPBACK) && f->addr)
            put_route4(b, flags, f, f->addr & f->netmask, prefix_len(f->netmask), 0);
}

long change_route(bool add, const u8* p, usize len) {
    if (len < sizeof(RtMsg))
        return -err::INVAL;
    RtMsg rt;
    memcpy(&rt, p, sizeof(rt));
    if (rt.family != AF_INET)
        return rt.family == AF_INET6 ? change_route6(add, p, len) : -EAFNOSUPPORT;
    u32 gw = 0;
    for_each_attr(p + sizeof(rt), len - sizeof(rt), [&](u16 type, const u8* data, usize alen) {
        if (type == RTA_GATEWAY && alen >= 4) {
            u32 v;
            memcpy(&v, data, 4);
            gw = be32(v);
        }
    });
    if (rt.dst_len != 0)
        return 0; // on-link subnet routes follow from the interface netmasks
    if (add) {
        if (!gw)
            return -err::INVAL;
        set_gateway(gw);
    } else if (!gw || gw == gateway()) {
        set_gateway(0);
    } else {
        return -err::SRCH;
    }
    return 0;
}

// ---- IPv6 addresses and routes ----

void dump_addr6(Builder& b) {
    for_each_addr6([](const Addr6& a, void* ctx) {
        auto& bb = *static_cast<Builder*>(ctx);
        struct {
            u8 family, prefixlen, flags, scope;
            u32 index;
        } ifa{AF_INET6, a.prefix_len, a.flags, a.scope, a.ifindex};
        bb.begin(RTM_NEWADDR, NLM_F_MULTI, &ifa, sizeof(ifa));
        bb.attr(IFA_ADDRESS, a.addr, 16);
        bb.attr_u32(IFA_FLAGS, a.flags);
        bb.end();
    }, &b);
}

long change_addr6(bool add, const u8* p, usize len) {
    u8 prefix = p[1];
    u32 index;
    memcpy(&index, p + 4, 4);
    u8 addr[16];
    bool have = false;
    for_each_attr(p + 8, len - 8, [&](u16 type, const u8* data, usize alen) {
        if ((type == IFA_LOCAL || (type == IFA_ADDRESS && !have)) && alen >= 16) {
            memcpy(addr, data, 16);
            have = true;
        }
    });
    if (!have)
        return -err::INVAL;
    return add ? add_addr6(index, addr, prefix) : del_addr6(index, addr);
}

void dump_routes6(Builder& b) {
    for_each_route6([](const Route6& r, void* ctx) {
        auto& bb = *static_cast<Builder*>(ctx);
        RtMsg rt{AF_INET6, r.dst_len, 0, 0, RT_TABLE_MAIN, r.protocol,
                 RT_SCOPE_UNIVERSE, RTN_UNICAST, 0};
        bb.begin(RTM_NEWROUTE, NLM_F_MULTI, &rt, sizeof(rt));
        bb.attr_u32(RTA_TABLE, RT_TABLE_MAIN);
        if (r.dst_len)
            bb.attr(RTA_DST, r.dst, 16);
        if (r.has_gateway)
            bb.attr(RTA_GATEWAY, r.gateway, 16);
        bb.attr_u32(RTA_PRIORITY, r.has_gateway ? 1024 : 256);
        bb.attr_u32(RTA_OIF, r.ifindex);
        bb.end();
    }, &b);
}

long change_route6(bool add, const u8* p, usize len) {
    RtMsg rt;
    memcpy(&rt, p, sizeof(rt));
    u8 gw[16];
    bool have_gw = false;
    u32 oif = 0;
    for_each_attr(p + sizeof(rt), len - sizeof(rt), [&](u16 type, const u8* data, usize alen) {
        if (type == RTA_GATEWAY && alen >= 16) {
            memcpy(gw, data, 16);
            have_gw = true;
        } else if (type == RTA_OIF && alen >= 4) {
            memcpy(&oif, data, 4);
        }
    });
    if (rt.dst_len != 0)
        return 0; // on-link prefixes follow from the addresses
    if (!add)
        return set_default_route6(nullptr, 0);
    if (!have_gw)
        return -err::INVAL;
    return set_default_route6(gw, oif);
}

// ---- the socket ----

class NetlinkSocket : public Socket {
public:
    NetlinkSocket(int t, int proto) : Socket(t, proto, AF_NETLINK) {}

    NetlinkSocket* next = nullptr;
    u32 portid = 0, groups = 0;
    u64 mc_groups[2] = {}; // multicast memberships, group n = bit n - 1 (bind bitmask + NETLINK_ADD_MEMBERSHIP)
    bool registered = false;
    sched::Mutex qlock;
    Datagram* head = nullptr;
    Datagram** tail = &head;
    usize queued = 0;

    // Pick a port id: the process id if free, else a negative number (as Linux does).
    long assign_port(u32 wanted) {
        LockGuard guard(g_nl);
        if (registered)
            return wanted == 0 || wanted == portid ? 0 : -err::INVAL;
        if (wanted) {
            if (port_taken(wanted))
                return -EADDRINUSE;
            portid = wanted;
        } else {
            u32 pid = static_cast<u32>(proc::current() ? proc::current()->pid : 0);
            while (!pid || port_taken(pid))
                pid = static_cast<u32>(g_next_auto_port--);
            portid = pid;
        }
        registered = true;
        next = g_sockets;
        g_sockets = this;
        return 0;
    }

    bool port_taken(u32 id) {
        for (NetlinkSocket* s = g_sockets; s; s = s->next)
            if (s->protocol == protocol && s->portid == id)
                return true;
        return false;
    }

    long bind_addr(const SockAddr& sa) override {
        if (sa.family() != AF_NETLINK || sa.len < 12)
            return -err::INVAL;
        u32 pid, grp;
        memcpy(&pid, sa.data + 4, 4);
        memcpy(&grp, sa.data + 8, 4);
        if (long r = assign_port(pid); r < 0)
            return r;
        groups = grp; // rtnetlink events are never sent, but remember the subscription
        mc_groups[0] = (mc_groups[0] & ~0xFFFFFFFFull) | grp;
        bound = true;
        return 0;
    }

    long connect_addr(const SockAddr& sa, bool) override {
        if (sa.family() != AF_NETLINK && sa.family() != AF_UNSPEC)
            return -err::INVAL;
        connected = sa.family() == AF_NETLINK;
        return assign_port(0);
    }

    long get_name(SockAddr* out, bool peer) override {
        *out = SockAddr{};
        out->data[0] = AF_NETLINK;
        out->len = 12;
        if (!peer) {
            memcpy(out->data + 4, &portid, 4);
            memcpy(out->data + 8, &groups, 4);
        }
        return 0;
    }

    isize send_msg(const u8* data, usize len, int, const SockAddr* to, Control*, bool) override {
        if (long r = assign_port(0); r < 0)
            return r;
        if (to && to->len >= 8) {
            u32 dst;
            memcpy(&dst, to->data + 4, 4);
            if (dst != 0)
                return static_cast<isize>(len); // unicast to another program: nobody listens
        }
        if (protocol == NETLINK_GENERIC) {
            lkpi_genl_rcv(portid, data, len); // replies arrive through lkpi_sylux_netlink_deliver()
            return static_cast<isize>(len);
        }
        if (protocol != NETLINK_ROUTE)
            return static_cast<isize>(len);
        for (usize pos = 0; pos + sizeof(NlMsgHdr) <= len;) {
            NlMsgHdr h;
            memcpy(&h, data + pos, sizeof(h));
            if (h.len < sizeof(h) || pos + h.len > len)
                break;
            handle(h, data + pos + sizeof(h), h.len - sizeof(h));
            pos += align4(h.len);
        }
        return static_cast<isize>(len);
    }

    // Answer one rtnetlink request.
    void handle(const NlMsgHdr& h, const u8* p, usize len) {
        if (!(h.flags & NLM_F_REQUEST))
            return;
        auto* b = new Builder(h.seq, portid); // ~9 KiB: too big for the kernel stack
        bool dump = (h.flags & NLM_F_DUMP) == NLM_F_DUMP;
        long result = 0;
        bool replied = false;
        u8 family = len >= 1 ? p[0] : 0;
        {
            LockGuard guard(g_lock);
            switch (h.type) {
            case RTM_GETLINK:
                if (dump) {
                    for (u32 i = 1; Interface* f = interface_by_index(i); i++)
                        put_link(*b, f, NLM_F_MULTI);
                } else {
                    i32 index = 0;
                    if (len >= 8)
                        memcpy(&index, p + 4, 4);
                    Interface* f = len >= 16 ? find_link(index, p + 16, len - 16) : interface_by_index(index);
                    if (f) {
                        put_link(*b, f, 0);
                        replied = true;
                    } else {
                        result = -err::NODEV;
                    }
                }
                break;
            case RTM_NEWLINK:
            case RTM_SETLINK: result = set_link(p, len); break;
            case RTM_DELLINK: result = -EOPNOTSUPP_; break;
            case RTM_GETADDR:
                if (!dump) {
                    result = -EOPNOTSUPP_;
                    break;
                }
                if (family == AF_UNSPEC || family == AF_INET)
                    for (u32 i = 1; Interface* f = interface_by_index(i); i++)
                        if (f->addr)
                            put_addr4(*b, f, NLM_F_MULTI);
                if (family == AF_UNSPEC || family == AF_INET6)
                    dump_addr6(*b);
                break;
            case RTM_NEWADDR:
            case RTM_DELADDR: result = change_addr(h.type == RTM_NEWADDR, p, len); break;
            case RTM_GETROUTE:
                if (!dump) {
                    result = -EOPNOTSUPP_;
                    break;
                }
                if (family == AF_UNSPEC || family == AF_INET)
                    dump_routes4(*b, NLM_F_MULTI);
                if (family == AF_UNSPEC || family == AF_INET6)
                    dump_routes6(*b);
                break;
            case RTM_NEWROUTE:
            case RTM_DELROUTE: result = change_route(h.type == RTM_NEWROUTE, p, len); break;
            default:
                // Other dumps (neighbours, rules, qdiscs...): an empty answer.
                if (!dump)
                    result = -EOPNOTSUPP_;
                break;
            }
        }
        if (dump && result == 0)
            b->done(NLM_F_MULTI);
        else if (result < 0 || ((h.flags & NLM_F_ACK) && !replied))
            b->error(static_cast<i32>(result), h);
        queue(b->take());
        delete b;
    }

    void queue(Datagram* list) {
        {
            LockGuard guard(qlock);
            while (list) {
                Datagram* d = list;
                list = d->next;
                d->next = nullptr;
                if (queued + d->len > QUEUE_LIMIT) { // reader asleep: drop, like a full socket buffer
                    kfree(d);
                    continue;
                }
                *tail = d;
                tail = &d->next;
                queued += d->len;
            }
        }
        wake();
    }

    isize recv_msg(u8* buf, usize len, int flags, SockAddr* from, Control* ctl, bool nonblock,
                   usize* full_len) override {
        for (;;) {
            if (long r = sock_wait(this, nonblock, rcv_timeout_ns, [&] { return head != nullptr; }); r < 0)
                return r;
            Datagram* d;
            {
                LockGuard guard(qlock);
                d = head;
                if (!d)
                    continue;
                if (!(flags & MSG_PEEK)) {
                    head = d->next;
                    if (!head)
                        tail = &head;
                    queued -= d->len;
                }
            }
            usize n = min(len, d->len);
            memcpy(buf, d->data, n);
            if (full_len)
                *full_len = d->len;
            if (from) { // from the kernel: port id 0
                *from = SockAddr{};
                from->data[0] = AF_NETLINK;
                from->len = 12;
                memcpy(from->data + 8, &d->src_group, 4);
            }
            if (ctl && d->cred) {
                ctl->has_cred = true;
                ctl->cred = Ucred{};
            }
            if (!(flags & MSG_PEEK))
                kfree(d);
            return static_cast<isize>(n);
        }
    }

    isize send(const u8* data, usize len, int flags, const Endpoint*, bool nonblock) override {
        return send_msg(data, len, flags, nullptr, nullptr, nonblock);
    }
    isize recv(u8* buf, usize len, int flags, Endpoint*, bool nonblock, usize* full_len) override {
        return recv_msg(buf, len, flags, nullptr, nullptr, nonblock, full_len);
    }

    u16 poll_mask() override { return ::poll::POLLOUT | (head ? ::poll::POLLIN : 0); }
    usize bytes_available() override {
        LockGuard guard(qlock);
        return head ? head->len : 0;
    }

    long set_option(int level, int name, const u8* val, u32 len) override {
        if (level != 270)
            return OPT_UNHANDLED;
        // SOL_NETLINK: NETLINK_ADD_MEMBERSHIP (1) / DROP (2); extended acks,
        // strict checks & co. are accepted and ignored.
        if ((name == 1 || name == 2) && len >= 4) {
            u32 group;
            memcpy(&group, val, 4);
            if (group == 0 || group > 128)
                return -err::INVAL;
            LockGuard guard(g_nl);
            u64 bit = 1ull << ((group - 1) % 64);
            if (name == 1)
                mc_groups[(group - 1) / 64] |= bit;
            else
                mc_groups[(group - 1) / 64] &= ~bit;
        }
        return 0;
    }

    bool member_of(u32 group) const {
        return group >= 1 && group <= 128 && (mc_groups[(group - 1) / 64] & (1ull << ((group - 1) % 64)));
    }
    long get_option(int level, int, u8* val, u32* len) override {
        if (level != 270)
            return OPT_UNHANDLED;
        memset(val, 0, 4);
        *len = 4;
        return 0;
    }

    void close() override {
        bool was_registered;
        {
            LockGuard guard(g_nl);
            NetlinkSocket** link = &g_sockets;
            while (*link && *link != this)
                link = &(*link)->next;
            if (*link)
                *link = next;
            was_registered = registered;
        }
        if (was_registered && protocol == NETLINK_GENERIC)
            lkpi_netlink_release(protocol, portid); // nl80211 drops what this socket owned
        while (head) {
            Datagram* d = head;
            head = d->next;
            kfree(d);
        }
        delete this;
    }
};

} // namespace

Socket* create_netlink(int type, int protocol) { return new NetlinkSocket(type, protocol); }

namespace {

Datagram* make_datagram(const void* data, usize len) {
    auto* d = static_cast<Datagram*>(kmalloc(sizeof(Datagram) + len));
    if (d) {
        d->next = nullptr;
        d->len = len;
        d->src_group = 0;
        d->cred = false;
        memcpy(d->data, data, len);
    }
    return d;
}

} // namespace

} // namespace net

// Messages from the kernel side of generic netlink (the LinuxKPI's
// genetlink: nl80211 replies and events). Called in process context.
extern "C" int lkpi_sylux_netlink_deliver(int protocol, u32 portid, const void* data, usize len) {
    using namespace net;
    LockGuard guard(g_nl);
    for (NetlinkSocket* s = g_sockets; s; s = s->next) {
        if (s->protocol == protocol && s->portid == portid) {
            if (Datagram* d = make_datagram(data, len))
                s->queue(d);
            return 0;
        }
    }
    return -111; // ECONNREFUSED: nobody listens on that port
}

extern "C" void lkpi_sylux_netlink_multicast(int protocol, u32 group, u32 exclude_portid, const void* data, usize len) {
    using namespace net;
    LockGuard guard(g_nl);
    for (NetlinkSocket* s = g_sockets; s; s = s->next)
        if (s->protocol == protocol && s->member_of(group) && (exclude_portid == 0 || s->portid != exclude_portid))
            if (Datagram* d = make_datagram(data, len))
                s->queue(d);
}

// ---- uevents ----

namespace net {

namespace {

constexpr int NETLINK_KOBJECT_UEVENT = 15;
constexpr u32 GROUP_KERNEL = 1, GROUP_UDEV = 2;
u64 g_seqnum = 0;

// systemd's string_hash32 (MurmurHash2, seed 0): libudev's socket filters
// compare these.
u32 murmur2(const char* s) {
    const u32 m = 0x5bd1e995;
    u32 len = static_cast<u32>(strlen(s)), h = len;
    auto* p = reinterpret_cast<const u8*>(s);
    for (; len >= 4; p += 4, len -= 4) {
        u32 k = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24);
        k *= m;
        k ^= k >> 24;
        k *= m;
        h = (h * m) ^ k;
    }
    switch (len) {
    case 3: h ^= p[2] << 16; [[fallthrough]];
    case 2: h ^= p[1] << 8; [[fallthrough]];
    case 1: h ^= p[0]; h *= m;
    }
    h ^= h >> 13;
    h *= m;
    return h ^ (h >> 15);
}

struct Env {
    char buf[2048];
    usize len = 0;
    void add(const char* s) {
        usize n = strlen(s) + 1;
        if (len + n <= sizeof(buf)) {
            memcpy(buf + len, s, n);
            len += n;
        }
    }
    void addf(const char* key, const char* value) {
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%s=%s", key, value);
        add(tmp);
    }
};

void multicast(u32 group, const void* data, usize len) {
    LockGuard guard(g_nl);
    for (NetlinkSocket* s = g_sockets; s; s = s->next)
        if (s->protocol == NETLINK_KOBJECT_UEVENT && s->member_of(group))
            if (Datagram* d = make_datagram(data, len)) {
                d->src_group = group;
                d->cred = true;
                s->queue(d);
            }
}

} // namespace

void uevent_broadcast(const char* action, const char* devpath, const char* subsystem, const char* const* kernel_env,
                      const char* const* udev_env, const char* const* tags) {
    char seq[24];
    snprintf(seq, sizeof(seq), "%lu", __atomic_add_fetch(&g_seqnum, 1, __ATOMIC_RELAXED));
    auto* k = new Env;
    auto* u = new Env;
    if (!k || !u) {
        delete k;
        delete u;
        return;
    }
    // Kernel format: "<action>@<devpath>", then KEY=VALUE strings.
    char head[192];
    snprintf(head, sizeof(head), "%s@%s", action, devpath);
    k->add(head);
    k->addf("ACTION", action);
    k->addf("DEVPATH", devpath);
    k->addf("SUBSYSTEM", subsystem);
    for (const char* const* e = kernel_env; e && *e; e++)
        k->add(*e);
    k->addf("SEQNUM", seq);
    multicast(GROUP_KERNEL, k->buf, k->len);

    // udevd's format: a "libudev" header, then the properties after
    // processing (ID_INPUT..., DEVNAME=/dev/..., TAGS).
    u->addf("ACTION", action);
    u->addf("DEVPATH", devpath);
    u->addf("SUBSYSTEM", subsystem);
    u->addf("SEQNUM", seq);
    char usec[24];
    snprintf(usec, sizeof(usec), "%lu", clock::now_ns() / 1000);
    u->addf("USEC_INITIALIZED", usec);
    for (const char* const* e = udev_env; e && *e; e++)
        u->add(*e);
    u64 bloom = 0;
    if (tags && *tags) {
        char list[128] = ":";
        for (const char* const* t = tags; *t; t++) {
            usize l = strlen(list);
            snprintf(list + l, sizeof(list) - l, "%s:", *t);
            u32 h = murmur2(*t);
            bloom |= (1ull << (h & 63)) | (1ull << ((h >> 6) & 63)) | (1ull << ((h >> 12) & 63)) |
                     (1ull << ((h >> 18) & 63));
        }
        u->addf("TAGS", list);
        u->addf("CURRENT_TAGS", list);
    }
    struct Header {
        char prefix[8];
        u32 magic, header_size, properties_off, properties_len, subsystem_hash, devtype_hash, tag_bloom_hi,
            tag_bloom_lo;
    };
    static_assert(sizeof(Header) == 40);
    auto* msg = static_cast<u8*>(kmalloc(sizeof(Header) + u->len));
    if (msg) {
        Header h;
        memcpy(h.prefix, "libudev", 8);
        h.magic = be32(0xFEEDCAFE);
        h.header_size = sizeof(Header); // these three in host order, as libudev writes them
        h.properties_off = sizeof(Header);
        h.properties_len = static_cast<u32>(u->len);
        h.subsystem_hash = be32(murmur2(subsystem));
        h.devtype_hash = 0;
        h.tag_bloom_hi = be32(static_cast<u32>(bloom >> 32));
        h.tag_bloom_lo = be32(static_cast<u32>(bloom));
        memcpy(msg, &h, sizeof(h));
        memcpy(msg + sizeof(h), u->buf, u->len);
        multicast(GROUP_UDEV, msg, sizeof(Header) + u->len);
        kfree(msg);
    }
    delete k;
    delete u;
}

} // namespace net

