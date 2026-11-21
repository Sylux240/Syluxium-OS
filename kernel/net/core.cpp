// Network core: interfaces, loopback, Ethernet, ARP, IPv4 and routing.

#include <core/log.hpp>
#include <lib/format.hpp>
#include <mm/heap.hpp>
#include <net/internal.hpp>
#include <sched/sched.hpp>
#include <time/clock.hpp>

namespace net {

sched::Mutex g_lock;

namespace {

constexpr u16 ETH_IPV4 = 0x0800, ETH_ARP = 0x0806;
constexpr usize MAX_IFACES = 16;

Interface* g_ifaces[MAX_IFACES];
u32 g_iface_count = 0;
u32 g_eth_count = 0;
u32 g_gateway = 0;
u16 g_ip_id = 1;

// ---- ARP cache ----

struct PendingPacket {
    u8* data; // IPv4 packet
    usize len;
};

struct ArpEntry {
    u32 ip = 0;
    u8 mac[6] = {};
    bool resolved = false;
    Interface* iface = nullptr;
    u64 updated_ms = 0, last_request_ms = 0;
    int requests = 0;
    PendingPacket pending[16] = {}; // room for a fragmented datagram
    int pending_count = 0;
};

constexpr usize ARP_SIZE = 64;
ArpEntry g_arp[ARP_SIZE];

ArpEntry* arp_find(u32 ip) {
    for (auto& e : g_arp)
        if (e.ip == ip)
            return &e;
    return nullptr;
}

ArpEntry* arp_slot(u32 ip) {
    if (ArpEntry* e = arp_find(ip))
        return e;
    ArpEntry* oldest = &g_arp[0];
    for (auto& e : g_arp) {
        if (!e.ip)
            return &e;
        if (e.updated_ms < oldest->updated_ms)
            oldest = &e;
    }
    for (int i = 0; i < oldest->pending_count; i++)
        kfree(oldest->pending[i].data);
    *oldest = ArpEntry{};
    return oldest;
}

void arp_send(Interface* iface, u16 op, const u8* target_mac, u32 target_ip) {
    u8 p[28];
    put16(p, 1);          // Ethernet
    put16(p + 2, ETH_IPV4);
    p[4] = 6;
    p[5] = 4;
    put16(p + 6, op);
    memcpy(p + 8, iface->mac, 6);
    put32(p + 14, iface->addr);
    memcpy(p + 18, op == 1 ? reinterpret_cast<const u8*>("\0\0\0\0\0\0") : target_mac, 6);
    put32(p + 24, target_ip);
    static const u8 broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    eth_send(iface, op == 1 ? broadcast : target_mac, ETH_ARP, p, sizeof(p));
}

void arp_input(Interface* iface, const u8* p, usize len) {
    if (len < 28 || get16(p) != 1 || get16(p + 2) != ETH_IPV4 || p[4] != 6 || p[5] != 4)
        return;
    u16 op = get16(p + 6);
    u32 sender_ip = get32(p + 14), target_ip = get32(p + 24);
    if (!iface->addr)
        return;
    // Learn/refresh the sender if it's someone we talk to or it's asking us.
    ArpEntry* e = arp_find(sender_ip);
    if (!e && target_ip == iface->addr && sender_ip)
        e = arp_slot(sender_ip);
    if (e) {
        e->ip = sender_ip;
        memcpy(e->mac, p + 8, 6);
        e->resolved = true;
        e->iface = iface;
        e->updated_ms = clock::now_ms();
        for (int i = 0; i < e->pending_count; i++) {
            eth_send(iface, e->mac, ETH_IPV4, e->pending[i].data, e->pending[i].len);
            kfree(e->pending[i].data);
        }
        e->pending_count = 0;
    }
    if (op == 1 && target_ip == iface->addr)
        arp_send(iface, 2, p + 8, sender_ip);
}

// ---- loopback ----

class Loopback : public Interface {
public:
    Loopback() {
        snprintf(name, sizeof(name), "lo");
        mtu = 65536;
        flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING;
        addr = LOOPBACK_ADDR;
        netmask = 0xFF000000;
    }
    long transmit(const u8* frame, usize len) override {
        u8* copy = static_cast<u8*>(kmalloc(len));
        if (!copy)
            return -err::NOMEM;
        memcpy(copy, frame, len);
        {
            LockGuard guard(m_lock);
            if (m_count == QUEUE) {
                kfree(copy);
                return -NOBUFS_;
            }
            m_queue[(m_head + m_count) % QUEUE] = {copy, len};
            m_count++;
        }
        m_ready.signal();
        return 0;
    }
    void run() {
        for (;;) {
            m_ready.wait();
            PendingPacket p;
            {
                LockGuard guard(m_lock);
                if (!m_count)
                    continue;
                p = m_queue[m_head];
                m_head = (m_head + 1) % QUEUE;
                m_count--;
            }
            receive(this, p.data, p.len);
            kfree(p.data);
        }
    }

private:
    static constexpr long NOBUFS_ = 105;
    static constexpr usize QUEUE = 256;
    PendingPacket m_queue[QUEUE];
    usize m_head = 0, m_count = 0;
    IrqSpinlock m_lock;
    sched::Semaphore m_ready;
};

Loopback* g_loopback = nullptr;

// Loopback frames carry no real Ethernet addresses.
const u8 zero_mac[6] = {};

Interface* route(u32 dst, u32* next_hop) {
    if ((dst >> 24) == 127 || is_local_address(dst)) {
        *next_hop = dst;
        return g_loopback;
    }
    if (is_mcast4(dst)) { // senders normally name the interface (IP_MULTICAST_IF)
        *next_hop = dst;
        return mc_interface(false, 0);
    }
    for (u32 i = 0; i < g_iface_count; i++) {
        Interface* f = g_ifaces[i];
        if (f == g_loopback || !f->addr || !(f->flags & IFF_UP))
            continue;
        if ((dst & f->netmask) == (f->addr & f->netmask) || dst == 0xFFFFFFFF) {
            *next_hop = dst;
            return f;
        }
    }
    if (g_gateway) {
        for (u32 i = 0; i < g_iface_count; i++) {
            Interface* f = g_ifaces[i];
            if (f != g_loopback && f->addr && (g_gateway & f->netmask) == (f->addr & f->netmask)) {
                *next_hop = g_gateway;
                return f;
            }
        }
    }
    // Unconfigured interface (DHCP): broadcasts go out of the first Ethernet one.
    if (dst == 0xFFFFFFFF)
        for (u32 i = 0; i < g_iface_count; i++)
            if (g_ifaces[i] != g_loopback) {
                *next_hop = dst;
                return g_ifaces[i];
            }
    return nullptr;
}

void reassemble4(Interface* iface, const u8* p, usize hl, const u8* data, usize dlen);

void ip_input(Interface* iface, const u8* p, usize len) {
    if (len < 20 || (p[0] >> 4) != 4)
        return;
    usize hl = (p[0] & 0xF) * 4u;
    usize total = get16(p + 2);
    if (hl < 20 || total < hl || total > len)
        return;
    if (iface != g_loopback && checksum_finish(checksum_add(0, p, hl)) != 0)
        return;
    u32 dst = get32(p + 16);
    bool for_us = iface->addr == 0 || dst == iface->addr || dst == 0xFFFFFFFF || is_local_address(dst) ||
                  (iface->netmask && dst == (iface->addr | ~iface->netmask)) || iface == g_loopback ||
                  (is_mcast4(dst) && mc_joined(iface->index, ep4(dst))) ||
                  // DHCP replies to the address being offered (a restarted client still has the old one)
                  (p[9] == PROTO_UDP && total >= hl + 4 && get16(p + hl + 2) == 68);
    if (!for_us)
        return;
    const u8* data = p + hl;
    usize dlen = total - hl;
    u16 frag = get16(p + 6);
    if ((frag & 0x2000) || (frag & 0x1FFF)) {
        reassemble4(iface, p, hl, data, dlen);
        return;
    }
    Endpoint src = ep4(get32(p + 12)), to = ep4(dst);
    raw_deliver(iface, p, total);
    switch (p[9]) {
    case 2: igmp_input(iface, data, dlen); break;
    case PROTO_ICMP: icmp_input(iface, p, total, data, dlen); break;
    case PROTO_UDP: udp_input(iface, p, total, src, to, data, dlen); break;
    case PROTO_TCP: tcp_input(iface, src, to, data, dlen); break;
    default: break;
    }
}

// Collect an IPv4 fragment; once the datagram is whole, rebuild it as one
// unfragmented packet (20-byte header) and process it again.
void reassemble4(Interface* iface, const u8* p, usize hl, const u8* data, usize dlen) {
    u16 frag = get16(p + 6);
    FragKey key{};
    key.v6 = false;
    memcpy(key.src, p + 12, 4);
    memcpy(key.dst, p + 16, 4);
    key.id = get16(p + 4);
    key.proto = p[9];
    usize total;
    u8* payload = frag_add(key, (frag & 0x1FFFu) * 8u, frag & 0x2000, data, dlen, &total);
    if (!payload)
        return;
    u8* whole = static_cast<u8*>(kmalloc(20 + total));
    if (whole) {
        memcpy(whole, p, 20);
        whole[0] = 0x45;
        put16(whole + 2, static_cast<u16>(20 + total));
        put16(whole + 6, 0);
        put16(whole + 10, 0);
        put16(whole + 10, checksum_finish(checksum_add(0, whole, 20)));
        memcpy(whole + 20, payload, total);
        if (20 + total <= 0xFFFF)
            ip_input(iface, whole, 20 + total);
        kfree(whole);
    }
    kfree(payload);
    (void)hl;
}

// rtnetlink events for link and IPv4 address changes, whoever made them
// (DHCP, ip/ifconfig, drivers reporting carrier from any context): the
// timer compares each interface with what it last saw (lock held).
struct IfSeen {
    bool known;
    u32 flags;
    bool link;
    u32 addr, mask;
};
IfSeen g_seen[MAX_IFACES];

void report_changes() {
    for (u32 i = 0; i < g_iface_count; i++) {
        Interface* f = g_ifaces[i];
        IfSeen& s = g_seen[i];
        IfSeen now{true, f->flags & IFF_UP, f->link_up, f->addr, f->netmask};
        if (s.known && (s.flags != now.flags || s.link != now.link))
            rtnl_link_event(f);
        if (s.known && (s.addr != now.addr || s.mask != now.mask)) {
            if (s.addr)
                rtnl_addr4_event(f, false, s.addr, s.mask);
            if (now.addr)
                rtnl_addr4_event(f, true, now.addr, now.mask);
        }
        s = now;
    }
}

void timer_thread(void*) {
    for (u32 tick = 0;; tick++) {
        sched::sleep_ms(10);
        u64 now = clock::now_ms();
        LockGuard guard(g_lock);
        if (tick % 10 == 0)
            report_changes();
        tcp_timer(now);
        ipv6_timer(now);
        frag_timer(now);
        for (auto& e : g_arp) {
            if (!e.ip || e.resolved || !e.iface)
                continue;
            if (now - e.last_request_ms >= 1000) {
                if (++e.requests > 3) { // give up: drop what was waiting
                    for (int i = 0; i < e.pending_count; i++)
                        kfree(e.pending[i].data);
                    e = ArpEntry{};
                    continue;
                }
                e.last_request_ms = now;
                arp_send(e.iface, 1, nullptr, e.ip);
            }
        }
    }
}

void loopback_thread(void*) { g_loopback->run(); }

} // namespace

void eth_send(Interface* iface, const u8* dst_mac, u16 type, const u8* payload, usize len) {
    u8 frame[MAX_FRAME];
    if (len > MAX_FRAME - 14)
        return;
    memcpy(frame, dst_mac, 6);
    memcpy(frame + 6, iface->mac, 6);
    put16(frame + 12, type);
    memcpy(frame + 14, payload, len);
    usize total = 14 + len;
    if (total < 60) { // minimum Ethernet frame
        memset(frame + total, 0, 60 - total);
        total = 60;
    }
    if (iface->transmit(frame, total) == 0) {
        iface->tx_packets++;
        iface->tx_bytes += total;
    } else {
        iface->tx_errors++;
    }
}

Interface* loopback_interface() { return g_loopback; }

// ---- checksums ----

u32 checksum_add(u32 sum, const void* data, usize len) {
    auto* p = static_cast<const u8*>(data);
    while (len > 1) {
        sum += static_cast<u32>((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += static_cast<u32>(p[0] << 8);
    return sum;
}

u16 checksum_finish(u32 sum) {
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<u16>(~sum);
}

u16 pseudo_checksum(u32 src, u32 dst, u8 proto, const u8* segment, usize len) {
    u8 pseudo[12];
    put32(pseudo, src);
    put32(pseudo + 4, dst);
    pseudo[8] = 0;
    pseudo[9] = proto;
    put16(pseudo + 10, static_cast<u16>(len));
    return checksum_finish(checksum_add(checksum_add(0, pseudo, 12), segment, len));
}

// ---- interfaces ----

bool g_started = false; // start() ran: interfaces registered later (hot-plug) are set up at once

void register_interface(Interface* iface) {
    if (g_iface_count >= MAX_IFACES)
        return;
    if (!iface->name[0])
        snprintf(iface->name, sizeof(iface->name), "eth%u", g_eth_count++);
    iface->index = g_iface_count + 1;
    g_ifaces[g_iface_count++] = iface;
    if (!(iface->flags & IFF_LOOPBACK))
        klog::ok("Network: %s, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s", iface->name, iface->mac[0], iface->mac[1],
                 iface->mac[2], iface->mac[3], iface->mac[4], iface->mac[5], iface->link_up ? "up" : "down");
    if (g_started && !(iface->flags & IFF_LOOPBACK)) {
        {
            LockGuard guard(g_lock);
            ipv6_init_interface(iface);
        }
        dhcp_start(iface);
    }
}

void link_up(Interface* iface) {
    iface->link_up = true;
    dhcp_kick(iface);
}

void unregister_interface(Interface* iface) {
    iface->detach_driver(); // waits for a transmit in progress, then no more calls
    LockGuard guard(g_lock);
    iface->detached = true;
    iface->flags &= ~(IFF_UP | IFF_RUNNING);
    iface->link_up = false;
    iface->addr = 0;
    iface->netmask = 0;
}

Interface* interface_by_index(u32 index) { return index >= 1 && index <= g_iface_count ? g_ifaces[index - 1] : nullptr; }

Interface* interface_by_name(const char* name) {
    for (u32 i = 0; i < g_iface_count; i++)
        if (strcmp(g_ifaces[i]->name, name) == 0)
            return g_ifaces[i];
    return nullptr;
}

void for_each_interface(void (*fn)(Interface*, void*), void* ctx) {
    for (u32 i = 0; i < g_iface_count; i++)
        fn(g_ifaces[i], ctx);
}

u32 gateway() { return g_gateway; }
void set_gateway(u32 gw) { g_gateway = gw; }

bool is_local_address(u32 addr) {
    if ((addr >> 24) == 127)
        return true;
    for (u32 i = 0; i < g_iface_count; i++)
        if (g_ifaces[i]->addr && g_ifaces[i]->addr == addr)
            return true;
    return false;
}

u32 source_for(u32 dst) {
    u32 hop;
    Interface* f = route(dst, &hop);
    if (!f)
        return 0;
    if (f == g_loopback)
        return (dst >> 24) == 127 ? LOOPBACK_ADDR : dst;
    return f->addr;
}

void format_ip(u32 ip, char* out, usize size) {
    snprintf(out, size, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

void receive(Interface* iface, const u8* frame, usize len) {
    if (len < 14)
        return;
    iface->rx_packets++;
    iface->rx_bytes += len;
    u16 type = get16(frame + 12);
    packet_deliver(iface, frame, len); // AF_PACKET listeners (EAPOL for wpa_supplicant, DHCP, tcpdump)
    LockGuard guard(g_lock);
    if (type == ETH_IPV4)
        ip_input(iface, frame + 14, len - 14);
    else if (type == ETH_IPV6)
        ip6_input(iface, frame + 14, len - 14);
    else if (type == ETH_ARP && iface != g_loopback)
        arp_input(iface, frame + 14, len - 14);
}

namespace {

// Send one finished IPv4 packet towards `hop` on `f` (lock held).
void ip_transmit(Interface* f, u32 hop, u32 dst, const u8* packet, usize total) {
    if (f == g_loopback) {
        eth_send(f, zero_mac, ETH_IPV4, packet, total);
        return;
    }
    if (dst == 0xFFFFFFFF || (f->netmask && dst == (f->addr | ~f->netmask))) {
        static const u8 broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        eth_send(f, broadcast, ETH_IPV4, packet, total);
        return;
    }
    if (is_mcast4(dst)) { // 01:00:5e + the group's low 23 bits (RFC 1112)
        const u8 mac[6] = {0x01, 0x00, 0x5E, static_cast<u8>((dst >> 16) & 0x7F), static_cast<u8>(dst >> 8),
                           static_cast<u8>(dst)};
        eth_send(f, mac, ETH_IPV4, packet, total);
        return;
    }
    ArpEntry* e = arp_find(hop);
    if (e && e->resolved) {
        eth_send(f, e->mac, ETH_IPV4, packet, total);
        return;
    }
    // Unknown next hop: queue the packet and ask who has it.
    e = arp_slot(hop);
    e->ip = hop;
    e->iface = f;
    if (e->pending_count < 16) {
        u8* copy = static_cast<u8*>(kmalloc(total));
        if (copy) {
            memcpy(copy, packet, total);
            e->pending[e->pending_count++] = {copy, total};
        }
    }
    u64 now = clock::now_ms();
    if (!e->updated_ms)
        e->updated_ms = now;
    if (now - e->last_request_ms >= 1000 || e->requests == 0) {
        e->last_request_ms = now;
        e->requests++;
        arp_send(f, 1, nullptr, hop);
    }
}

} // namespace

long ip_send(u32 src, u32 dst, u8 proto, const u8* payload, usize len, u8 ttl, Interface* via) {
    u32 hop = dst;
    Interface* f = via ? via : route(dst, &hop);
    if (!f)
        return -101; // ENETUNREACH
    if (len + 20 > MAX_IP_PAYLOAD)
        return -90; // EMSGSIZE
    if (!src)
        src = f == g_loopback ? ((dst >> 24) == 127 ? LOOPBACK_ADDR : dst) : f->addr;
    usize mtu = min<usize>(f->mtu, MAX_FRAME - 14);
    // Packets that fit go out whole with DF set (path MTU discovery style);
    // bigger ones are cut into fragments of a multiple of 8 bytes.
    bool fragment = len + 20 > mtu;
    usize chunk = fragment ? ((mtu - 20) & ~usize(7)) : len;
    u16 id = g_ip_id++;
    u8 packet[MAX_FRAME];
    usize off = 0;
    do {
        usize n = min(chunk, len - off);
        bool more = off + n < len;
        u8* h = packet;
        h[0] = 0x45;
        h[1] = 0;
        put16(h + 2, static_cast<u16>(20 + n));
        put16(h + 4, id);
        put16(h + 6, fragment ? static_cast<u16>((more ? 0x2000 : 0) | (off / 8)) : 0x4000);
        h[8] = ttl;
        h[9] = proto;
        put16(h + 10, 0);
        put32(h + 12, src);
        put32(h + 16, dst);
        put16(h + 10, checksum_finish(checksum_add(0, h, 20)));
        memcpy(packet + 20, payload + off, n);
        ip_transmit(f, hop, dst, packet, 20 + n);
        off += n;
    } while (off < len);
    return 0;
}

// A complete IPv4 packet from a raw socket (IP_HDRINCL): routed by its
// destination; the checksum, length and id are filled in as Linux does.
long ip_send_raw(const u8* packet, usize len) {
    if (len < 20 || len > MAX_FRAME - 14 || (packet[0] >> 4) != 4)
        return -err::INVAL;
    usize hl = (packet[0] & 0xF) * 4u;
    if (hl < 20 || hl > len)
        return -err::INVAL;
    u32 dst = get32(packet + 16), hop = dst;
    Interface* f = route(dst, &hop);
    if (!f)
        return -101; // ENETUNREACH
    if (len > min<usize>(f->mtu, MAX_FRAME - 14))
        return -90; // EMSGSIZE
    u8 copy[MAX_FRAME];
    memcpy(copy, packet, len);
    put16(copy + 2, static_cast<u16>(len));
    if (!get16(copy + 4))
        put16(copy + 4, g_ip_id++);
    if (!get32(copy + 12))
        put32(copy + 12, f->addr);
    put16(copy + 10, 0);
    put16(copy + 10, checksum_finish(checksum_add(0, copy, hl)));
    ip_transmit(f, hop, dst, copy, len);
    return 0;
}

// ---- family-neutral helpers for TCP/UDP ----

u16 pseudo_checksum6(const u8* src, const u8* dst, u8 proto, const u8* segment, usize len) {
    u8 pseudo[40];
    memcpy(pseudo, src, 16);
    memcpy(pseudo + 16, dst, 16);
    put32(pseudo + 32, static_cast<u32>(len));
    pseudo[36] = pseudo[37] = pseudo[38] = 0;
    pseudo[39] = proto;
    return checksum_finish(checksum_add(checksum_add(0, pseudo, 40), segment, len));
}

u16 l4_checksum(const Endpoint& src, const Endpoint& dst, u8 proto, const u8* segment, usize len) {
    if (dst.v6)
        return pseudo_checksum6(src.addr6, dst.addr6, proto, segment, len);
    return pseudo_checksum(src.addr, dst.addr, proto, segment, len);
}

long ip_output(const Endpoint& src, const Endpoint& dst, u8 proto, const u8* seg, usize len) {
    if (dst.v6)
        return ip6_send(src.any() ? nullptr : src.addr6, dst.addr6, dst.scope, proto, seg, len);
    return ip_send(src.addr, dst.addr, proto, seg, len);
}

bool source_endpoint(const Endpoint& dst, Endpoint* src) {
    *src = Endpoint{};
    if (dst.v6) {
        src->v6 = true;
        src->scope = dst.scope;
        return source6_for(dst.addr6, dst.scope, src->addr6);
    }
    src->addr = source_for(dst.addr);
    return src->addr != 0 || dst.addr == 0xFFFFFFFF;
}

bool is_local_endpoint(const Endpoint& e) {
    return e.v6 ? is_local_address6(e.addr6) : is_local_address(e.addr);
}

u32 path_mss(const Endpoint& dst) {
    // Every interface (loopback included) is limited to 1500-byte packets by our frame buffers.
    return dst.v6 ? 1500 - 60 : 1500 - 40;
}

void init() {
    g_loopback = new Loopback;
    register_interface(g_loopback);
    proc_net_init();
}

void start(u64 dhcp_wait_ms) {
    g_started = true;
    sched::spawn("net-timer", timer_thread, nullptr, -1, true);
    sched::spawn("net-loopback", loopback_thread, nullptr, -1, true);
    {
        LockGuard guard(g_lock);
        for (u32 i = 0; i < g_iface_count; i++)
            ipv6_init_interface(g_ifaces[i]); // ::1, link-local addresses, router solicitation
    }
    bool any = false;
    for (u32 i = 0; i < g_iface_count; i++) {
        if (g_ifaces[i] != g_loopback) {
            dhcp_start(g_ifaces[i]);
            any = true;
        }
    }
    if (!any)
        return;
    u64 deadline = clock::now_ms() + dhcp_wait_ms;
    while (!dhcp_configured() && clock::now_ms() < deadline)
        sched::sleep_ms(20);
    if (!dhcp_configured())
        klog::warn("Network: no DHCP answer yet (still trying in the background)");
}

} // namespace net
