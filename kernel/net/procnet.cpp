// /proc/net/{dev,route,tcp,udp,arp} and /etc/resolv.conf, in Linux's
// formats so ifconfig, route, netstat and the resolver read them.

#include <fs/genfs.hpp>
#include <net/internal.hpp>

namespace net {

namespace {

using genfs::TextBuffer;

bool gen_dev(TextBuffer& out, const void*) {
    out.printf("Inter-|   Receive                                                |  Transmit\n");
    out.printf(" face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo "
               "colls carrier compressed\n");
    LockGuard guard(g_lock);
    for_each_interface([](Interface* f, void* ctx) {
        static_cast<TextBuffer*>(ctx)->printf("%6s: %7lu %7lu    0 %4lu    0     0          0         0 %8lu %7lu "
                                              "%4lu    0    0     0       0          0\n",
                                              f->name, f->rx_bytes, f->rx_packets, f->rx_dropped, f->tx_bytes,
                                              f->tx_packets, f->tx_errors);
    }, &out);
    return true;
}

bool gen_route(TextBuffer& out, const void*) {
    out.printf("Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
    LockGuard guard(g_lock);
    struct Ctx {
        TextBuffer* out;
        u32 gw;
        bool gw_done;
    } ctx{&out, gateway(), false};
    for_each_interface([](Interface* f, void* p) {
        auto* c = static_cast<Ctx*>(p);
        if ((f->flags & IFF_LOOPBACK) || !f->addr)
            return;
        // Addresses in /proc/net/route are little-endian hex (network order read as a host u32).
        if (c->gw && !c->gw_done && (c->gw & f->netmask) == (f->addr & f->netmask)) {
            c->out->printf("%s\t00000000\t%08X\t0003\t0\t0\t0\t00000000\t0\t0\t0\n", f->name, be32(c->gw));
            c->gw_done = true;
        }
        c->out->printf("%s\t%08X\t00000000\t0001\t0\t0\t0\t%08X\t0\t0\t0\n", f->name, be32(f->addr & f->netmask),
                       be32(f->netmask));
    }, &ctx);
    return true;
}

bool gen_tcp(TextBuffer& out, const void*) {
    out.printf("  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    LockGuard guard(g_lock);
    struct Ctx {
        TextBuffer* out;
        int n;
    } ctx{&out, 0};
    for_each_tcp([](Socket* s, int state, void* p) {
        auto* c = static_cast<Ctx*>(p);
        if (s->domain != AF_INET)
            return;
        c->out->printf("%4d: %08X:%04X %08X:%04X %02X 00000000:00000000 00:00000000 00000000     0        0 %lu\n",
                       c->n++, be32(s->local.addr), s->local.port, be32(s->remote.addr), s->remote.port, state, s->ino);
    }, &ctx);
    return true;
}

bool gen_udp(TextBuffer& out, const void*) {
    out.printf("  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    LockGuard guard(g_lock);
    struct Ctx {
        TextBuffer* out;
        int n;
    } ctx{&out, 0};
    for_each_udp([](Socket* s, void* p) {
        auto* c = static_cast<Ctx*>(p);
        if (!s->bound || s->domain != AF_INET)
            return;
        c->out->printf("%4d: %08X:%04X %08X:%04X %02X 00000000:00000000 00:00000000 00000000     0        0 %lu\n",
                       c->n++, be32(s->local.addr), s->local.port, be32(s->remote.addr), s->remote.port,
                       s->connected ? 1 : 7, s->ino);
    }, &ctx);
    return true;
}

// An endpoint as /proc/net/tcp6 shows it: four 32-bit words, each the
// network-order bytes read as a little-endian number; IPv4 as v4-mapped.
void put_addr6(TextBuffer& out, const Endpoint& ep) {
    u8 a[16] = {};
    if (ep.v6) {
        memcpy(a, ep.addr6, 16);
    } else if (ep.addr) {
        a[10] = a[11] = 0xFF;
        put32(a + 12, ep.addr);
    }
    for (int i = 0; i < 4; i++)
        out.printf("%02X%02X%02X%02X", a[4 * i + 3], a[4 * i + 2], a[4 * i + 1], a[4 * i]);
    out.printf(":%04X", ep.port);
}

struct Ctx6 {
    TextBuffer* out;
    int n;
};

void line6(Ctx6* c, Socket* s, int state) {
    c->out->printf("%4d: ", c->n++);
    put_addr6(*c->out, s->local);
    c->out->printf(" ");
    put_addr6(*c->out, s->remote);
    c->out->printf(" %02X 00000000:00000000 00:00000000 00000000     0        0 %lu\n", state, s->ino);
}

const char* const HEADER6 = "  sl  local_address                         remote_address                        st "
                            "tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n";

bool gen_tcp6(TextBuffer& out, const void*) {
    out.printf("%s", HEADER6);
    LockGuard guard(g_lock);
    Ctx6 ctx{&out, 0};
    for_each_tcp([](Socket* s, int state, void* p) {
        if (s->domain == AF_INET6)
            line6(static_cast<Ctx6*>(p), s, state);
    }, &ctx);
    return true;
}

bool gen_udp6(TextBuffer& out, const void*) {
    out.printf("%s", HEADER6);
    LockGuard guard(g_lock);
    Ctx6 ctx{&out, 0};
    for_each_udp([](Socket* s, void* p) {
        if (s->domain == AF_INET6 && s->bound)
            line6(static_cast<Ctx6*>(p), s, s->connected ? 1 : 7);
    }, &ctx);
    return true;
}

// /proc/net/if_inet6: what ifconfig reads to show IPv6 addresses.
bool gen_if_inet6(TextBuffer& out, const void*) {
    LockGuard guard(g_lock);
    for_each_addr6([](const Addr6& a, void* p) {
        auto* o = static_cast<TextBuffer*>(p);
        for (u8 b : a.addr)
            o->printf("%02x", b);
        Interface* f = interface_by_index(a.ifindex);
        u8 scope = a.scope == 254 ? 0x10 : a.scope == 253 ? 0x20 : 0x00;
        o->printf(" %02x %02x %02x %02x %8s\n", a.ifindex, a.prefix_len, scope, a.flags, f ? f->name : "?");
    }, &out);
    return true;
}

bool gen_resolv(TextBuffer& out, const void*) {
    out.printf("# Generated by the Syluxium kernel from DHCP\n");
    u32 dns = dns_server();
    if (dns) {
        char ip[16];
        format_ip(dns, ip, sizeof(ip));
        out.printf("nameserver %s\n", ip);
    }
    u8 dns6[16];
    bool have6;
    {
        LockGuard guard(g_lock);
        have6 = dns_server6(dns6);
    }
    if (have6) {
        char ip6[48];
        format_ip6(dns6, ip6, sizeof(ip6));
        out.printf("nameserver %s\n", ip6);
    }
    return true;
}

} // namespace

void proc_net_init() {
    vfs::mkdir_p("/proc/net");
    vfs::add_node("/proc/net/dev", new genfs::GeneratedFile(gen_dev, nullptr));
    vfs::add_node("/proc/net/route", new genfs::GeneratedFile(gen_route, nullptr));
    vfs::add_node("/proc/net/tcp", new genfs::GeneratedFile(gen_tcp, nullptr));
    vfs::add_node("/proc/net/udp", new genfs::GeneratedFile(gen_udp, nullptr));
    vfs::add_node("/proc/net/tcp6", new genfs::GeneratedFile(gen_tcp6, nullptr));
    vfs::add_node("/proc/net/udp6", new genfs::GeneratedFile(gen_udp6, nullptr));
    vfs::add_node("/proc/net/if_inet6", new genfs::GeneratedFile(gen_if_inet6, nullptr));
    unix_proc_init();
    sysnet_init(); // /sys/class/net, /proc/sys/net/ipv4/conf
    // /run/resolv.conf (the system's /etc/resolv.conf links to it, as on
    // Debian with resolvconf): /run stays in RAM when "/" moves to a disk
    // (root=), /etc doesn't. Also at /etc/resolv.conf for the RAM root.
    vfs::mkdir_p("/run");
    vfs::add_node("/run/resolv.conf", new genfs::GeneratedFile(gen_resolv, nullptr, 0644));
    vfs::mkdir_p("/etc");
    vfs::Inode* existing;
    if (vfs::resolve("/etc/resolv.conf", nullptr, false, &existing) != 0)
        vfs::add_node("/etc/resolv.conf", new genfs::GeneratedFile(gen_resolv, nullptr, 0644));
}

} // namespace net
