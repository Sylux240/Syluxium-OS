// DHCP client (RFC 2131), run by the kernel for every Ethernet interface,
// like Linux's ip=dhcp: DISCOVER -> OFFER -> REQUEST -> ACK, then renew at
// half the lease. Configures address, netmask, default route and DNS.

#include <core/log.hpp>
#include <lib/format.hpp>
#include <net/internal.hpp>
#include <sched/sched.hpp>
#include <time/clock.hpp>

namespace net {

namespace {

constexpr u32 MAGIC = 0x63825363;
constexpr u8 DISCOVER = 1, OFFER = 2, REQUEST = 3, ACK = 5, NAK = 6;

struct Client {
    Interface* iface = nullptr;
    u32 xid = 0;
    sched::Semaphore reply_ready;
    u8 reply[576];
    usize reply_len = 0;
    bool waiting = false;
    volatile bool kick = false; // link (re)gained: start over now
};

constexpr usize MAX_CLIENTS = 4;
Client g_clients[MAX_CLIENTS];
usize g_client_count = 0;
bool g_configured = false;
u32 g_dns = 0;

void send_packet(Client* c, u8 type, u32 requested, u32 server) {
    u8 pkt[300] = {};
    pkt[0] = 1; // BOOTREQUEST
    pkt[1] = 1; // Ethernet
    pkt[2] = 6;
    put32(pkt + 4, c->xid);
    put16(pkt + 10, 0x8000); // reply by broadcast: we have no address yet
    memcpy(pkt + 28, c->iface->mac, 6);
    put32(pkt + 236, MAGIC);
    u8* o = pkt + 240;
    *o++ = 53; *o++ = 1; *o++ = type;
    *o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, c->iface->mac, 6); o += 6; // client id
    *o++ = 12; *o++ = 8; memcpy(o, "syluxium", 8); o += 8;               // host name
    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;       // want: mask, router, DNS, lease
    if (requested) { *o++ = 50; *o++ = 4; put32(o, requested); o += 4; }
    if (server) { *o++ = 54; *o++ = 4; put32(o, server); o += 4; }
    *o++ = 255;

    u8 udp[8 + sizeof(pkt)];
    put16(udp, 68);
    put16(udp + 2, 67);
    put16(udp + 4, sizeof(udp));
    put16(udp + 6, 0);
    memcpy(udp + 8, pkt, sizeof(pkt));
    put16(udp + 6, pseudo_checksum(0, 0xFFFFFFFF, PROTO_UDP, udp, sizeof(udp)));
    LockGuard guard(g_lock);
    ip_send(0, 0xFFFFFFFF, PROTO_UDP, udp, sizeof(udp), 64, c->iface);
}

struct Reply {
    u8 type = 0;
    u32 yiaddr = 0, server = 0, mask = 0, router = 0, dns = 0, lease = 0;
};

bool wait_reply(Client* c, u8 want1, u8 want2, Reply* out, u64 timeout_ms) {
    u64 deadline = clock::now_ms() + timeout_ms;
    for (;;) {
        u64 now = clock::now_ms();
        if (now >= deadline || c->kick)
            return false;
        if (!c->reply_ready.wait_timeout((deadline - now) * 1'000'000))
            return false;
        if (c->kick)
            return false;
        const u8* p = c->reply;
        usize len = c->reply_len;
        if (len < 240 || p[0] != 2 || get32(p + 4) != c->xid || get32(p + 236) != MAGIC)
            continue;
        Reply r;
        r.yiaddr = get32(p + 16);
        for (usize i = 240; i < len;) {
            u8 code = p[i];
            if (code == 255)
                break;
            if (code == 0) {
                i++;
                continue;
            }
            if (i + 1 >= len)
                break;
            u8 olen = p[i + 1];
            const u8* v = p + i + 2;
            if (i + 2 + olen > len)
                break;
            switch (code) {
            case 53: if (olen >= 1) r.type = v[0]; break;
            case 1: if (olen >= 4) r.mask = get32(v); break;
            case 3: if (olen >= 4) r.router = get32(v); break;
            case 6: if (olen >= 4) r.dns = get32(v); break;
            case 51: if (olen >= 4) r.lease = get32(v); break;
            case 54: if (olen >= 4) r.server = get32(v); break;
            default: break;
            }
            i += 2u + olen;
        }
        if (r.type == want1 || r.type == want2) {
            *out = r;
            return true;
        }
    }
}

void client_thread(void* arg) {
    auto* c = static_cast<Client*>(arg);
    u64 backoff = 1000;
    for (;;) {
        if (c->kick) {
            c->kick = false;
            backoff = 1000;
        }
        if (!c->iface->link_up || !(c->iface->flags & IFF_UP)) {
            // No link yet (Wi-Fi not associated, cable out): wait for link_up().
            c->reply_ready.wait_timeout(2000ull * 1'000'000);
            continue;
        }
        c->xid = static_cast<u32>(clock::now_ns() ^ (c->iface->mac[5] << 24));
        send_packet(c, DISCOVER, 0, 0);
        Reply offer;
        if (!wait_reply(c, OFFER, OFFER, &offer, backoff)) {
            backoff = min<u64>(backoff * 2, 16000);
            continue;
        }
        send_packet(c, REQUEST, offer.yiaddr, offer.server);
        Reply ack;
        if (!wait_reply(c, ACK, NAK, &ack, 3000) || ack.type == NAK) {
            sched::sleep_ms(1000);
            continue;
        }
        backoff = 1000;
        {
            LockGuard guard(g_lock);
            c->iface->addr = ack.yiaddr;
            c->iface->netmask = ack.mask ? ack.mask : 0xFFFFFF00;
            if (ack.router)
                set_gateway(ack.router);
            if (ack.dns)
                g_dns = ack.dns;
        }
        char ip[16], gw[16], dns[16];
        format_ip(ack.yiaddr, ip, sizeof(ip));
        format_ip(ack.router, gw, sizeof(gw));
        format_ip(ack.dns, dns, sizeof(dns));
        klog::ok("DHCP: %s is %s/%d, gateway %s, DNS %s, lease %us", c->iface->name, ip,
                 __builtin_popcount(c->iface->netmask), gw, dns, ack.lease);
        g_configured = true;

        // Renew at half the lease (simplified: a fresh REQUEST for the same address).
        u64 lease = ack.lease ? ack.lease : 3600;
        for (;;) {
            u64 renew_at = clock::now_ms() + lease * 500;
            while (!c->kick && clock::now_ms() < renew_at)
                c->reply_ready.wait_timeout((renew_at - clock::now_ms()) * 1'000'000);
            if (c->kick)
                break; // new link (maybe another network): start over
            c->xid++;
            send_packet(c, REQUEST, ack.yiaddr, ack.server);
            Reply renew;
            if (wait_reply(c, ACK, NAK, &renew, 5000) && renew.type == ACK) {
                lease = renew.lease ? renew.lease : lease;
                continue;
            }
            break; // lost it: start over
        }
    }
}

} // namespace

void dhcp_start(Interface* iface) {
    if (g_client_count >= MAX_CLIENTS)
        return;
    Client* c = &g_clients[g_client_count++];
    c->iface = iface;
    sched::spawn("dhcp", client_thread, c, -1, true);
}

void dhcp_input(Interface* in, const u8* data, usize len) {
    for (usize i = 0; i < g_client_count; i++) {
        Client* c = &g_clients[i];
        if (c->iface != in || len > sizeof(c->reply))
            continue;
        memcpy(c->reply, data, len);
        c->reply_len = len;
        c->reply_ready.signal();
    }
}

void dhcp_kick(Interface* iface) {
    for (usize i = 0; i < g_client_count; i++) {
        Client* c = &g_clients[i];
        if (c->iface == iface) {
            c->kick = true;
            c->reply_ready.signal();
        }
    }
}

bool dhcp_configured() { return g_configured; }
u32 dns_server() { return g_dns; }

} // namespace net
