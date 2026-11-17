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
    PendingPacket pending[4] = {};
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

void ip_input(Interface* iface, const u8* p, usize len) {
    if (len < 20 || (p[0] >> 4) != 4)
        return;
    usize hl = (p[0] & 0xF) * 4u;
    usize total = get16(p + 2);
    if (hl < 20 || total < hl || total > len)
        return;
    if (iface != g_loopback && checksum_finish(checksum_add(0, p, hl)) != 0)
        return;
    u16 frag = get16(p + 6);
    if ((frag & 0x2000) || (frag & 0x1FFF)) {
        iface->rx_dropped++; // fragments: reassembly not implemented
        return;
    }
    u32 dst = get32(p + 16);
    bool for_us = iface->addr == 0 || dst == iface->addr || dst == 0xFFFFFFFF || is_local_address(dst) ||
                  (iface->netmask && dst == (iface->addr | ~iface->netmask)) || iface == g_loopback;
    if (!for_us)
        return;
    const u8* data = p + hl;
    usize dlen = total - hl;
    switch (p[9]) {
    case PROTO_ICMP: icmp_input(iface, p, total, data, dlen); break;
    case PROTO_UDP: udp_input(iface, p, data, dlen); break;
    case PROTO_TCP: tcp_input(iface, p, data, dlen); break;
    default: break;
    }
}

void timer_thread(void*) {
    for (;;) {
        sched::sleep_ms(10);
        u64 now = clock::now_ms();
        LockGuard guard(g_lock);
        tcp_timer(now);
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
    LockGuard guard(g_lock);
    if (type == ETH_IPV4)
        ip_input(iface, frame + 14, len - 14);
    else if (type == ETH_ARP && iface != g_loopback)
        arp_input(iface, frame + 14, len - 14);
}

long ip_send(u32 src, u32 dst, u8 proto, const u8* payload, usize len, u8 ttl) {
    u32 hop;
    Interface* f = route(dst, &hop);
    if (!f)
        return -101; // ENETUNREACH
    if (len + 20 > f->mtu || len + 20 > MAX_FRAME - 14)
        return -90; // EMSGSIZE
    u8 packet[MAX_FRAME];
    u8* h = packet;
    h[0] = 0x45;
    h[1] = 0;
    put16(h + 2, static_cast<u16>(20 + len));
    put16(h + 4, g_ip_id++);
    put16(h + 6, 0x4000); // don't fragment
    h[8] = ttl;
    h[9] = proto;
    put16(h + 10, 0);
    if (!src)
        src = f == g_loopback ? ((dst >> 24) == 127 ? LOOPBACK_ADDR : dst) : f->addr;
    put32(h + 12, src);
    put32(h + 16, dst);
    put16(h + 10, checksum_finish(checksum_add(0, h, 20)));
    memcpy(packet + 20, payload, len);
    usize total = 20 + len;

    if (f == g_loopback) {
        eth_send(f, zero_mac, ETH_IPV4, packet, total);
        return 0;
    }
    if (dst == 0xFFFFFFFF || (f->netmask && dst == (f->addr | ~f->netmask))) {
        static const u8 broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        eth_send(f, broadcast, ETH_IPV4, packet, total);
        return 0;
    }
    ArpEntry* e = arp_find(hop);
    if (e && e->resolved) {
        eth_send(f, e->mac, ETH_IPV4, packet, total);
        return 0;
    }
    // Unknown next hop: queue the packet and ask who has it.
    e = arp_slot(hop);
    e->ip = hop;
    e->iface = f;
    if (e->pending_count < 4) {
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
    return 0;
}

void init() {
    g_loopback = new Loopback;
    register_interface(g_loopback);
    proc_net_init();
}

void start(u64 dhcp_wait_ms) {
    sched::spawn("net-timer", timer_thread, nullptr, -1, true);
    sched::spawn("net-loopback", loopback_thread, nullptr, -1, true);
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
