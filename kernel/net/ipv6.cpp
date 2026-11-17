// IPv6 (RFC 8200) with neighbour discovery (RFC 4861), stateless address
// autoconfiguration (RFC 4862) and ICMPv6 (RFC 4443).
//
// Every Ethernet interface gets a link-local address (fe80::/64 + EUI-64
// interface id) and sends router solicitations; router advertisements
// provide a default router, on-link prefixes, global addresses (SLAAC) and
// DNS servers (RDNSS, RFC 8106). The loopback interface carries ::1.
// Fragmented packets are reassembled (frag.cpp) and big datagrams are
// fragmented on output.
//
// Not done: duplicate address detection (addresses are used right away),
// MLD (switches that snoop multicast may then filter our solicited-node
// traffic), privacy addresses, DHCPv6, address lifetimes other than the
// router's.

#include <core/log.hpp>
#include <lib/format.hpp>
#include <mm/heap.hpp>
#include <net/internal.hpp>
#include <time/clock.hpp>

namespace net {

namespace {

constexpr u8 NH_HOPOPTS = 0, NH_ROUTING = 43, NH_FRAGMENT = 44, NH_DSTOPTS = 60;
constexpr u8 ICMP6_UNREACH = 1, ICMP6_ECHO_REQUEST = 128, ICMP6_ECHO_REPLY = 129, ND_ROUTER_SOLICIT = 133,
             ND_ROUTER_ADVERT = 134, ND_NEIGHBOR_SOLICIT = 135, ND_NEIGHBOR_ADVERT = 136, ND_REDIRECT = 137;
constexpr u8 OPT_SOURCE_LL = 1, OPT_TARGET_LL = 2, OPT_PREFIX = 3, OPT_RDNSS = 25;
constexpr u8 SCOPE_GLOBAL = 0, SCOPE_LINK = 253, SCOPE_HOST = 254;
constexpr u8 IFA_F_PERMANENT = 0x80;
constexpr u8 RTPROT_KERNEL = 2, RTPROT_STATIC = 4, RTPROT_RA = 9;

const u8 ALL_NODES[16] = {0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
const u8 ALL_ROUTERS[16] = {0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
const u8 LOOPBACK6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
const u8 zero_mac[6] = {};

// ---- addresses, prefixes, router ----

struct AddrEntry {
    bool used = false;
    u32 ifindex = 0;
    u8 addr[16] = {};
    u8 prefix = 0;
    bool dynamic = false; // from SLAAC
};
constexpr usize MAX_ADDRS = 32;
AddrEntry g_addrs[MAX_ADDRS];

struct OnLink { // prefixes a router said are on-link (L flag)
    bool used = false;
    u32 ifindex = 0;
    u8 prefix[16] = {};
    u8 len = 0;
};
OnLink g_onlink[8];

struct DefaultRouter {
    bool used = false;
    u8 addr[16] = {};
    u32 ifindex = 0;
    u64 expires_ms = 0; // 0 = never (configured by hand)
    u8 protocol = RTPROT_RA;
} g_router;

u8 g_dns6[16];
bool g_has_dns6 = false;

// Router solicitation state per interface (indexed by ifindex).
struct IfState {
    int rs_sent = 0;
    u64 next_rs_ms = 0;
    bool got_ra = false;
};
IfState g_if[17];

bool prefix_match(const u8* a, const u8* b, u8 len) {
    usize bytes = len / 8;
    if (memcmp(a, b, bytes) != 0)
        return false;
    u8 bits = len % 8;
    if (!bits)
        return true;
    u8 mask = static_cast<u8>(0xFF << (8 - bits));
    return (a[bytes] & mask) == (b[bytes] & mask);
}

void make_interface_id(const Interface* f, u8* out8) { // modified EUI-64
    out8[0] = f->mac[0] ^ 0x02;
    out8[1] = f->mac[1];
    out8[2] = f->mac[2];
    out8[3] = 0xFF;
    out8[4] = 0xFE;
    out8[5] = f->mac[3];
    out8[6] = f->mac[4];
    out8[7] = f->mac[5];
}

void solicited_node(const u8* target, u8* out) {
    static const u8 base[13] = {0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xFF};
    memcpy(out, base, 13);
    memcpy(out + 13, target + 13, 3);
}

AddrEntry* find_addr(const u8* a) {
    for (auto& e : g_addrs)
        if (e.used && memcmp(e.addr, a, 16) == 0)
            return &e;
    return nullptr;
}

// The interface's link-local (or, with `global`, best global) address.
const u8* iface_addr(u32 ifindex, bool global) {
    for (auto& e : g_addrs)
        if (e.used && e.ifindex == ifindex && ip6::is_link_local(e.addr) != global)
            return e.addr;
    return nullptr;
}

void format6(const u8* a, char* out, usize size) {
    // Compress the longest run of zero groups, as RFC 5952 asks.
    u16 g[8];
    for (int i = 0; i < 8; i++)
        g[i] = get16(a + 2 * i);
    int best = -1, best_len = 0;
    for (int i = 0; i < 8;) {
        if (g[i]) {
            i++;
            continue;
        }
        int j = i;
        while (j < 8 && !g[j])
            j++;
        if (j - i > best_len && j - i > 1) {
            best = i;
            best_len = j - i;
        }
        i = j;
    }
    usize pos = 0;
    for (int i = 0; i < 8 && pos + 6 < size;) {
        if (i == best) {
            pos += static_cast<usize>(snprintf(out + pos, size - pos, i == 0 ? "::" : ":"));
            i += best_len;
            continue;
        }
        pos += static_cast<usize>(snprintf(out + pos, size - pos, "%x%s", g[i], i < 7 ? ":" : ""));
        i++;
    }
    if (pos && out[pos - 1] == ':' && best + best_len != 8)
        out[--pos] = 0;
}

long add_addr_entry(u32 ifindex, const u8* addr, u8 prefix, bool dynamic) {
    if (find_addr(addr))
        return -err::EXIST;
    for (auto& e : g_addrs) {
        if (e.used)
            continue;
        e.used = true;
        e.ifindex = ifindex;
        memcpy(e.addr, addr, 16);
        e.prefix = prefix;
        e.dynamic = dynamic;
        return 0;
    }
    return -ENOBUFS;
}

// ---- neighbour cache (the IPv6 ARP) ----

struct Pending {
    u8* data;
    usize len;
};

struct Neighbor {
    bool used = false;
    u8 ip[16] = {};
    u8 mac[6] = {};
    bool resolved = false;
    Interface* iface = nullptr;
    u64 updated_ms = 0, last_request_ms = 0;
    int requests = 0;
    Pending pending[16] = {};
    int pending_count = 0;
};
constexpr usize NC_SIZE = 64;
Neighbor g_nc[NC_SIZE];

Neighbor* nc_find(const u8* ip) {
    for (auto& n : g_nc)
        if (n.used && memcmp(n.ip, ip, 16) == 0)
            return &n;
    return nullptr;
}

void nc_clear(Neighbor& n) {
    for (int i = 0; i < n.pending_count; i++)
        kfree(n.pending[i].data);
    n = Neighbor{};
}

Neighbor* nc_slot(const u8* ip) {
    if (Neighbor* n = nc_find(ip))
        return n;
    Neighbor* oldest = &g_nc[0];
    for (auto& n : g_nc) {
        if (!n.used) {
            oldest = &n;
            break;
        }
        if (n.updated_ms < oldest->updated_ms)
            oldest = &n;
    }
    nc_clear(*oldest);
    oldest->used = true;
    memcpy(oldest->ip, ip, 16);
    oldest->updated_ms = clock::now_ms();
    return oldest;
}

// Record a neighbour's link-layer address; send what was waiting for it.
void nc_learn(Interface* f, const u8* ip, const u8* mac, bool create) {
    Neighbor* n = nc_find(ip);
    if (!n && !create)
        return;
    if (!n)
        n = nc_slot(ip);
    memcpy(n->mac, mac, 6);
    n->resolved = true;
    n->iface = f;
    n->updated_ms = clock::now_ms();
    n->requests = 0;
    for (int i = 0; i < n->pending_count; i++) {
        eth_send(f, n->mac, ETH_IPV6, n->pending[i].data, n->pending[i].len);
        kfree(n->pending[i].data);
    }
    n->pending_count = 0;
}

// ---- ICMPv6 output ----

long icmp6_send(const u8* src, const u8* dst, u32 scope, u8* msg, usize len, u8 hop_limit) {
    u8 s[16];
    if (src) {
        memcpy(s, src, 16);
    } else if (!source6_for(dst, scope, s)) {
        return -EADDRNOTAVAIL;
    }
    put16(msg + 2, 0);
    put16(msg + 2, pseudo_checksum6(s, dst, PROTO_ICMPV6, msg, len));
    return ip6_send(s, dst, scope, PROTO_ICMPV6, msg, len, hop_limit);
}

void send_neighbor_solicit(Interface* f, const u8* target) {
    const u8* src = iface_addr(f->index, false);
    if (!src)
        return;
    u8 msg[32] = {};
    msg[0] = ND_NEIGHBOR_SOLICIT;
    memcpy(msg + 8, target, 16);
    msg[24] = OPT_SOURCE_LL;
    msg[25] = 1;
    memcpy(msg + 26, f->mac, 6);
    u8 dst[16];
    solicited_node(target, dst);
    icmp6_send(src, dst, f->index, msg, sizeof(msg), 255);
}

void send_router_solicit(Interface* f) {
    const u8* src = iface_addr(f->index, false);
    if (!src)
        return;
    u8 msg[16] = {};
    msg[0] = ND_ROUTER_SOLICIT;
    msg[8] = OPT_SOURCE_LL;
    msg[9] = 1;
    memcpy(msg + 10, f->mac, 6);
    icmp6_send(src, ALL_ROUTERS, f->index, msg, sizeof(msg), 255);
}

// ---- routing ----

Interface* first_ethernet() {
    for (u32 i = 1; Interface* f = interface_by_index(i); i++)
        if (!(f->flags & IFF_LOOPBACK))
            return f;
    return nullptr;
}

// Which interface (and next hop) reaches `dst`?
Interface* route6(const u8* dst, u32 scope, u8* hop) {
    memcpy(hop, dst, 16);
    if (ip6::is_loopback(dst) || is_local_address6(dst))
        return loopback_interface();
    if (ip6::is_multicast(dst) || ip6::is_link_local(dst)) {
        Interface* f = scope ? interface_by_index(scope) : first_ethernet();
        return f && (f->flags & IFF_UP) ? f : nullptr;
    }
    for (auto& e : g_addrs) {
        Interface* f = e.used ? interface_by_index(e.ifindex) : nullptr;
        if (f && !(f->flags & IFF_LOOPBACK) && (f->flags & IFF_UP) && !ip6::is_link_local(e.addr) &&
            prefix_match(dst, e.addr, e.prefix))
            return f;
    }
    for (auto& p : g_onlink) {
        Interface* f = p.used ? interface_by_index(p.ifindex) : nullptr;
        if (f && (f->flags & IFF_UP) && prefix_match(dst, p.prefix, p.len))
            return f;
    }
    if (g_router.used) {
        Interface* f = interface_by_index(g_router.ifindex);
        if (f && (f->flags & IFF_UP)) {
            memcpy(hop, g_router.addr, 16);
            return f;
        }
    }
    return nullptr;
}

// Put one finished packet on the wire towards `hop` (lock held).
void transmit6(Interface* f, const u8* hop, const u8* packet, usize len) {
    if (f == loopback_interface()) {
        eth_send(f, zero_mac, ETH_IPV6, packet, len);
        return;
    }
    if (ip6::is_multicast(hop)) { // 33:33 + low 32 bits of the group
        u8 mac[6] = {0x33, 0x33, hop[12], hop[13], hop[14], hop[15]};
        eth_send(f, mac, ETH_IPV6, packet, len);
        return;
    }
    Neighbor* n = nc_find(hop);
    if (n && n->resolved) {
        eth_send(f, n->mac, ETH_IPV6, packet, len);
        return;
    }
    n = nc_slot(hop);
    n->iface = f;
    if (n->pending_count < 16) {
        u8* copy = static_cast<u8*>(kmalloc(len));
        if (copy) {
            memcpy(copy, packet, len);
            n->pending[n->pending_count++] = {copy, len};
        }
    }
    u64 now = clock::now_ms();
    if (n->requests == 0 || now - n->last_request_ms >= 1000) {
        n->last_request_ms = now;
        n->requests++;
        send_neighbor_solicit(f, hop);
    }
}

u32 g_frag_id = 0x5EED0000;

// ---- ICMPv6 sockets (ping6: raw and SOCK_DGRAM "ping" sockets) ----

struct Packet {
    Endpoint from;
    usize len;
    Packet* next;
    u8 data[];
};

class Icmp6Socket;
Icmp6Socket* g_icmp6 = nullptr;
u16 g_next_echo_id = 0x6000;

class Icmp6Socket : public Socket {
public:
    explicit Icmp6Socket(int t) : Socket(t, PROTO_ICMPV6, AF_INET6) {
        LockGuard guard(g_lock);
        local.v6 = true;
        local.port = g_next_echo_id++; // ping sockets: the echo identifier
        next = g_icmp6;
        g_icmp6 = this;
    }

    Icmp6Socket* next = nullptr;
    Packet* head = nullptr;
    Packet* tail = nullptr;
    usize count = 0;

    long bind(const Endpoint& ep) override {
        if (!ep.v6)
            return -err::INVAL;
        LockGuard guard(g_lock);
        if (!ep.any() && !is_local_address6(ep.addr6))
            return -EADDRNOTAVAIL;
        u16 id = local.port;
        local = ep;
        local.port = (type == SOCK_DGRAM && ep.port) ? ep.port : id;
        bound = true;
        return 0;
    }

    long connect(const Endpoint& ep, bool) override {
        LockGuard guard(g_lock);
        if (!ep.v6 && !ep.any())
            return -err::INVAL;
        remote = ep;
        connected = !ep.any();
        return 0;
    }

    isize send(const u8* data, usize len, int, const Endpoint* to, bool) override {
        LockGuard guard(g_lock);
        Endpoint dst = to ? *to : remote;
        if (!to && !connected)
            return -EDESTADDRREQ;
        if (!dst.v6)
            return -err::INVAL;
        if (len < 4 || len > 1452)
            return len < 4 ? -err::INVAL : -EMSGSIZE;
        u8 msg[1452];
        memcpy(msg, data, len);
        if (type == SOCK_DGRAM && len >= 8)
            put16(msg + 4, local.port); // the kernel owns the echo identifier
        long r = icmp6_send(local.any() ? nullptr : local.addr6, dst.addr6, dst.scope, msg, len, 64);
        return r < 0 ? r : static_cast<isize>(len);
    }

    isize recv(u8* buf, usize len, int flags, Endpoint* from, bool nonblock, usize* full_len) override {
        for (;;) {
            if (long r = sock_wait(this, nonblock, rcv_timeout_ns, [&] { return head != nullptr; }); r < 0)
                return r;
            LockGuard guard(g_lock);
            Packet* p = head;
            if (!p)
                continue;
            usize n = min(len, p->len);
            memcpy(buf, p->data, n);
            if (from)
                *from = p->from;
            if (full_len)
                *full_len = p->len;
            if (!(flags & MSG_PEEK)) {
                head = p->next;
                if (!head)
                    tail = nullptr;
                count--;
                kfree(p);
            }
            return static_cast<isize>(n);
        }
    }

    u16 poll_mask() override { return ::poll::POLLOUT | (head ? ::poll::POLLIN : 0); }
    usize bytes_available() override { return head ? head->len : 0; }

    void deliver(const Endpoint& from, const u8* data, usize len) {
        if (count >= 64)
            return;
        auto* p = static_cast<Packet*>(kmalloc(sizeof(Packet) + len));
        if (!p)
            return;
        p->from = from;
        p->len = len;
        p->next = nullptr;
        memcpy(p->data, data, len);
        if (tail)
            tail->next = p;
        else
            head = p;
        tail = p;
        count++;
        wake();
    }

    void close() override {
        {
            LockGuard guard(g_lock);
            Icmp6Socket** link = &g_icmp6;
            while (*link && *link != this)
                link = &(*link)->next;
            if (*link)
                *link = next;
            while (head) {
                Packet* p = head;
                head = p->next;
                kfree(p);
            }
        }
        delete this;
    }
};

// ---- ICMPv6 input ----

// Walk the options of an ND message; fn(type, option, length in bytes).
template <typename Fn>
void for_each_option(const u8* opts, usize len, Fn fn) {
    for (usize pos = 0; pos + 2 <= len;) {
        usize olen = opts[pos + 1] * 8u;
        if (olen == 0 || pos + olen > len)
            return;
        fn(opts[pos], opts + pos, olen);
        pos += olen;
    }
}

void handle_neighbor_solicit(Interface* in, const u8* ip, const u8* data, usize len) {
    if (len < 24 || ip[7] != 255)
        return;
    const u8* src = ip + 8;
    const u8* target = data + 8;
    if (!is_local_address6(target))
        return;
    bool dad = ip6::is_unspecified(src);
    if (!dad)
        for_each_option(data + 24, len - 24, [&](u8 type, const u8* o, usize olen) {
            if (type == OPT_SOURCE_LL && olen >= 8)
                nc_learn(in, src, o + 2, true);
        });
    u8 msg[32] = {};
    msg[0] = ND_NEIGHBOR_ADVERT;
    msg[4] = dad ? 0x20 : 0x60; // Solicited (unless answering DAD) + Override
    memcpy(msg + 8, target, 16);
    msg[24] = OPT_TARGET_LL;
    msg[25] = 1;
    memcpy(msg + 26, in->mac, 6);
    icmp6_send(target, dad ? ALL_NODES : src, in->index, msg, sizeof(msg), 255);
}

void handle_neighbor_advert(Interface* in, const u8* ip, const u8* data, usize len) {
    if (len < 24 || ip[7] != 255)
        return;
    const u8* target = data + 8;
    for_each_option(data + 24, len - 24, [&](u8 type, const u8* o, usize olen) {
        if (type == OPT_TARGET_LL && olen >= 8)
            nc_learn(in, target, o + 2, false);
    });
}

void handle_router_advert(Interface* in, const u8* ip, const u8* data, usize len) {
    const u8* src = ip + 8;
    if (len < 16 || ip[7] != 255 || !ip6::is_link_local(src) || (in->flags & IFF_LOOPBACK))
        return;
    u16 lifetime = get16(data + 6);
    if (in->index < 17)
        g_if[in->index].got_ra = true;
    for_each_option(data + 16, len - 16, [&](u8 type, const u8* o, usize olen) {
        if (type == OPT_SOURCE_LL && olen >= 8) {
            nc_learn(in, src, o + 2, true);
        } else if (type == OPT_PREFIX && olen >= 32) {
            u8 plen = o[2], flags = o[3];
            u32 valid = get32(o + 4);
            const u8* prefix = o + 16;
            if (ip6::is_link_local(prefix) || plen > 128)
                return;
            if ((flags & 0x80) && valid) { // on-link
                bool known = false;
                for (auto& p : g_onlink)
                    known |= p.used && p.ifindex == in->index && p.len == plen && prefix_match(p.prefix, prefix, plen);
                for (auto& p : g_onlink)
                    if (!known && !p.used) {
                        p = OnLink{true, in->index, {}, plen};
                        memcpy(p.prefix, prefix, 16);
                        break;
                    }
            }
            if ((flags & 0x40) && plen == 64 && valid) { // autonomous: SLAAC
                u8 addr[16];
                memcpy(addr, prefix, 8);
                make_interface_id(in, addr + 8);
                if (!find_addr(addr) && add_addr_entry(in->index, addr, 64, true) == 0) {
                    char text[48];
                    format6(addr, text, sizeof(text));
                    klog::ok("Network: %s IPv6 address %s/64 (SLAAC)", in->name, text);
                }
            }
        } else if (type == OPT_RDNSS && olen >= 24) {
            if (get32(o + 4)) {
                memcpy(g_dns6, o + 8, 16);
                g_has_dns6 = true;
            }
        }
    });
    if (lifetime) {
        if (!g_router.used || g_router.protocol == RTPROT_RA) {
            g_router.used = true;
            memcpy(g_router.addr, src, 16);
            g_router.ifindex = in->index;
            g_router.expires_ms = clock::now_ms() + lifetime * 1000ull;
            g_router.protocol = RTPROT_RA;
        }
    } else if (g_router.used && memcmp(g_router.addr, src, 16) == 0) {
        g_router.used = false;
    }
}

} // namespace

// ---- public interface ----

bool is_local_address6(const u8* a) { return ip6::is_loopback(a) || find_addr(a) != nullptr; }

bool source6_for(const u8* dst, u32 scope, u8* out) {
    if (ip6::is_loopback(dst)) {
        memcpy(out, LOOPBACK6, 16);
        return true;
    }
    if (find_addr(dst)) {
        memcpy(out, dst, 16);
        return true;
    }
    u8 hop[16];
    Interface* f = route6(dst, scope, hop);
    if (!f)
        return false;
    bool link_scope = ip6::is_link_local(dst) || (ip6::is_multicast(dst) && (dst[1] & 0x0F) <= 2);
    const u8* a = link_scope ? nullptr : iface_addr(f->index, true);
    if (!a)
        a = iface_addr(f->index, false);
    if (!a)
        return false;
    memcpy(out, a, 16);
    return true;
}

long ip6_send(const u8* src, const u8* dst, u32 scope, u8 proto, const u8* payload, usize len, u8 hop_limit) {
    u8 hop[16];
    Interface* f = route6(dst, scope, hop);
    if (!f)
        return -ENETUNREACH;
    u8 s[16];
    if (src)
        memcpy(s, src, 16);
    else if (!source6_for(dst, scope, s))
        return -EADDRNOTAVAIL;
    if (len > MAX_IP_PAYLOAD - 8)
        return -EMSGSIZE;
    usize mtu = min<usize>(f->mtu, MAX_FRAME - 14);
    bool fragment = len + 40 > mtu;
    usize chunk = fragment ? ((mtu - 48) & ~usize(7)) : len;
    u32 id = g_frag_id++;
    u8 packet[MAX_FRAME];
    usize off = 0;
    do {
        usize n = min(chunk, len - off);
        bool more = off + n < len;
        usize hl = fragment ? 48 : 40;
        put32(packet, 0x60000000);
        put16(packet + 4, static_cast<u16>(hl - 40 + n));
        packet[6] = fragment ? NH_FRAGMENT : proto;
        packet[7] = hop_limit;
        memcpy(packet + 8, s, 16);
        memcpy(packet + 24, dst, 16);
        if (fragment) {
            packet[40] = proto;
            packet[41] = 0;
            put16(packet + 42, static_cast<u16>(off | (more ? 1 : 0)));
            put32(packet + 44, id);
        }
        memcpy(packet + hl, payload + off, n);
        transmit6(f, hop, packet, hl + n);
        off += n;
    } while (off < len);
    return 0;
}

void ip6_input(Interface* in, const u8* p, usize len) {
    if (len < 40 || (p[0] >> 4) != 6)
        return;
    usize end = 40 + get16(p + 4);
    if (end > len)
        return;
    const u8* src = p + 8;
    const u8* dst = p + 24;
    bool for_us = is_local_address6(dst) || memcmp(dst, ALL_NODES, 16) == 0 ||
                  (dst[0] == 0xFF && dst[1] == 0x01 && dst[15] == 1);
    if (!for_us && dst[0] == 0xFF && dst[1] == 0x02 && dst[11] == 1 && dst[12] == 0xFF) // solicited-node group
        for (auto& e : g_addrs)
            for_us |= e.used && e.ifindex == in->index && memcmp(e.addr + 13, dst + 13, 3) == 0;
    if (!for_us)
        return;
    // Link-local peers are only meaningful together with their interface.
    u32 scope = in->index;
    if (in == loopback_interface())
        if (AddrEntry* e = find_addr(dst))
            scope = e->ifindex;

    u8 nh = p[6];
    usize off = 40;
    for (;;) {
        switch (nh) {
        case NH_HOPOPTS:
        case NH_DSTOPTS:
        case NH_ROUTING: {
            if (off + 8 > end)
                return;
            usize hlen = (p[off + 1] + 1u) * 8u;
            if (nh == NH_ROUTING && p[off + 3] != 0)
                return; // segments left: we are not a router
            nh = p[off];
            off += hlen;
            continue;
        }
        case NH_FRAGMENT: {
            if (off + 8 > end)
                return;
            FragKey key{};
            key.v6 = true;
            memcpy(key.src, src, 16);
            memcpy(key.dst, dst, 16);
            key.id = get32(p + off + 4);
            key.proto = p[off];
            u16 fo = get16(p + off + 2);
            usize total;
            u8* payload = frag_add(key, fo & 0xFFF8u, fo & 1, p + off + 8, end - off - 8, &total);
            if (!payload)
                return;
            // Rebuild one unfragmented packet and process it again.
            if (u8* whole = static_cast<u8*>(kmalloc(40 + total))) {
                memcpy(whole, p, 40);
                put16(whole + 4, static_cast<u16>(total));
                whole[6] = key.proto;
                memcpy(whole + 40, payload, total);
                if (total <= 0xFFFF)
                    ip6_input(in, whole, 40 + total);
                kfree(whole);
            }
            kfree(payload);
            return;
        }
        case PROTO_ICMPV6:
            icmp6_input(in, p, p + off, end - off);
            return;
        case PROTO_UDP:
            udp_input(in, p, end, ep6(src, 0, scope), ep6(dst, 0, scope), p + off, end - off);
            return;
        case PROTO_TCP:
            tcp_input(in, ep6(src, 0, scope), ep6(dst, 0, scope), p + off, end - off);
            return;
        default:
            return;
        }
    }
}

void icmp6_input(Interface* in, const u8* ip, const u8* data, usize len) {
    if (len < 4)
        return;
    const u8* src = ip + 8;
    const u8* dst = ip + 24;
    if (!(in->flags & IFF_LOOPBACK) && pseudo_checksum6(src, dst, PROTO_ICMPV6, data, len) != 0)
        return;
    u8 type = data[0];
    switch (type) {
    case ICMP6_ECHO_REQUEST: {
        if (len > 1452 || len < 8)
            break;
        u8 reply[1452];
        memcpy(reply, data, len);
        reply[0] = ICMP6_ECHO_REPLY;
        icmp6_send(ip6::is_multicast(dst) ? nullptr : dst, src, in->index, reply, len, 64);
        break;
    }
    case ND_NEIGHBOR_SOLICIT: handle_neighbor_solicit(in, ip, data, len); break;
    case ND_NEIGHBOR_ADVERT: handle_neighbor_advert(in, ip, data, len); break;
    case ND_ROUTER_ADVERT: handle_router_advert(in, ip, data, len); break;
    default: break;
    }
    if (type >= ND_ROUTER_SOLICIT && type <= ND_REDIRECT)
        return; // neighbour discovery is the kernel's business
    u32 scope = ip6::is_link_local(src) ? in->index : 0;
    for (Icmp6Socket* s = g_icmp6; s; s = s->next) {
        if (s->type == SOCK_RAW)
            s->deliver(ep6(src, 0, scope), data, len);
        else if (type == ICMP6_ECHO_REPLY && len >= 8 && get16(data + 4) == s->local.port)
            s->deliver(ep6(src, 0, scope), data, len);
    }
}

void icmp6_unreachable(const u8* ip6, usize len, u8 code) {
    const u8* src = ip6 + 8;
    if (ip6::is_multicast(ip6 + 24) || ip6::is_unspecified(src))
        return; // never answer multicast (RFC 4443 2.4)
    usize quote = min<usize>(len, 1232 - 8);
    u8 msg[1232];
    memset(msg, 0, 8);
    msg[0] = ICMP6_UNREACH;
    msg[1] = code;
    memcpy(msg + 8, ip6, quote);
    icmp6_send(ip6 + 24, src, 0, msg, 8 + quote, 64);
}

Socket* create_icmp6(int type) { return new Icmp6Socket(type); }

void ipv6_init_interface(Interface* f) {
    if (f->flags & IFF_LOOPBACK) {
        add_addr_entry(f->index, LOOPBACK6, 128, false);
        return;
    }
    u8 ll[16] = {0xFE, 0x80};
    make_interface_id(f, ll + 8);
    add_addr_entry(f->index, ll, 64, false);
    char text[48];
    format6(ll, text, sizeof(text));
    klog::info("Network: %s IPv6 link-local address %s", f->name, text);
    if (f->index < 17) {
        g_if[f->index].next_rs_ms = clock::now_ms() + 50;
        g_if[f->index].rs_sent = 0;
    }
}

void ipv6_timer(u64 now) {
    // Router solicitations: up to 3, 4 s apart, until an advertisement arrives.
    for (u32 i = 1; i < 17; i++) {
        IfState& st = g_if[i];
        Interface* f = interface_by_index(i);
        if (!f || (f->flags & IFF_LOOPBACK) || st.got_ra || st.rs_sent >= 3 || !st.next_rs_ms || now < st.next_rs_ms)
            continue;
        st.rs_sent++;
        st.next_rs_ms = now + 4000;
        send_router_solicit(f);
    }
    if (g_router.used && g_router.expires_ms && now >= g_router.expires_ms)
        g_router.used = false;
    // Neighbour solicitations for unresolved entries; give up after 3.
    for (auto& n : g_nc) {
        if (!n.used || n.resolved || !n.iface)
            continue;
        if (now - n.last_request_ms >= 1000) {
            if (++n.requests > 3) {
                nc_clear(n);
                continue;
            }
            n.last_request_ms = now;
            send_neighbor_solicit(n.iface, n.ip);
        }
    }
}

void for_each_addr6(void (*fn)(const Addr6&, void*), void* ctx) {
    for (auto& e : g_addrs) {
        if (!e.used)
            continue;
        Addr6 a{};
        memcpy(a.addr, e.addr, 16);
        a.prefix_len = e.prefix;
        a.scope = ip6::is_loopback(e.addr) ? SCOPE_HOST : ip6::is_link_local(e.addr) ? SCOPE_LINK : SCOPE_GLOBAL;
        a.flags = e.dynamic ? 0 : IFA_F_PERMANENT;
        a.dynamic = e.dynamic;
        a.ifindex = e.ifindex;
        fn(a, ctx);
    }
}

void for_each_route6(void (*fn)(const Route6&, void*), void* ctx) {
    if (g_router.used) {
        Route6 r{};
        memcpy(r.gateway, g_router.addr, 16);
        r.has_gateway = true;
        r.ifindex = g_router.ifindex;
        r.protocol = g_router.protocol;
        fn(r, ctx);
    }
    // One on-link route per distinct (interface, prefix).
    for (usize i = 0; i < MAX_ADDRS; i++) {
        const AddrEntry& e = g_addrs[i];
        if (!e.used || e.prefix == 128)
            continue;
        bool dup = false;
        for (usize j = 0; j < i; j++)
            dup |= g_addrs[j].used && g_addrs[j].ifindex == e.ifindex && g_addrs[j].prefix == e.prefix &&
                   prefix_match(g_addrs[j].addr, e.addr, e.prefix);
        if (dup)
            continue;
        Route6 r{};
        for (usize b = 0; b < 16; b++) {
            int bits = static_cast<int>(e.prefix) - static_cast<int>(b * 8);
            u8 mask = bits >= 8 ? 0xFF : bits <= 0 ? 0 : static_cast<u8>(0xFF << (8 - bits));
            r.dst[b] = e.addr[b] & mask;
        }
        r.dst_len = e.prefix;
        r.ifindex = e.ifindex;
        r.protocol = RTPROT_KERNEL;
        fn(r, ctx);
    }
}

long add_addr6(u32 ifindex, const u8* addr, u8 prefix_len) {
    if (!interface_by_index(ifindex) || prefix_len > 128)
        return -err::INVAL;
    return add_addr_entry(ifindex, addr, prefix_len, false);
}

long del_addr6(u32 ifindex, const u8* addr) {
    AddrEntry* e = find_addr(addr);
    if (!e || e->ifindex != ifindex)
        return -EADDRNOTAVAIL;
    e->used = false;
    return 0;
}

long set_default_route6(const u8* gateway, u32 ifindex) {
    if (!gateway) {
        g_router.used = false;
        return 0;
    }
    if (!ifindex) {
        u8 hop[16];
        Interface* f = route6(gateway, 0, hop);
        if (!f)
            return -ENETUNREACH;
        ifindex = f->index;
    }
    g_router.used = true;
    memcpy(g_router.addr, gateway, 16);
    g_router.ifindex = ifindex;
    g_router.expires_ms = 0;
    g_router.protocol = RTPROT_STATIC;
    return 0;
}

bool dns_server6(u8* out) {
    if (g_has_dns6)
        memcpy(out, g_dns6, 16);
    return g_has_dns6;
}

void format_ip6(const u8* a, char* out, usize size) { format6(a, out, size); }

} // namespace net
