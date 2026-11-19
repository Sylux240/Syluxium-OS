#pragma once

// Shared internals of the TCP/IP stack (not for drivers).

#include <fs/vfs.hpp>
#include <lib/string.hpp>
#include <net/net.hpp>
#include <fs/poll.hpp>
#include <proc/signal.hpp>
#include <sched/sync.hpp>

namespace net {

inline u16 be16(u16 v) { return __builtin_bswap16(v); }
inline u32 be32(u32 v) { return __builtin_bswap32(v); }
inline u16 get16(const u8* p) { return static_cast<u16>((p[0] << 8) | p[1]); }
inline u32 get32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }
inline void put16(u8* p, u16 v) { p[0] = static_cast<u8>(v >> 8); p[1] = static_cast<u8>(v); }
inline void put32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v >> 24); p[1] = static_cast<u8>(v >> 16);
    p[2] = static_cast<u8>(v >> 8); p[3] = static_cast<u8>(v);
}

// Networking errno values (Linux numbers).
constexpr long EMSGSIZE = 90, EADDRINUSE = 98, EADDRNOTAVAIL = 99, ENETUNREACH = 101, ECONNRESET = 104,
               ENOBUFS = 105, EISCONN = 106, ENOTCONN = 107, ETIMEDOUT_ = 110, ECONNREFUSED = 111,
               EHOSTUNREACH = 113, EALREADY = 114, EINPROGRESS = 115, EDESTADDRREQ = 89, EOPNOTSUPP_ = 95;

class Socket;

constexpr u8 PROTO_ICMP = 1, PROTO_TCP = 6, PROTO_UDP = 17, PROTO_ICMPV6 = 58;
constexpr u32 LOOPBACK_ADDR = 0x7F000001;
constexpr usize MAX_FRAME = 1514;
constexpr usize MAX_IP_PAYLOAD = 65535; // largest datagram we fragment / reassemble

// One lock for the whole stack: protocol state, sockets, ARP. Held while
// processing a received packet or a socket call; never across sleeps.
extern sched::Mutex g_lock;

constexpr u16 AF_UNSPEC = 0, AF_INET = 2, AF_INET6 = 10;

// An address + port, IPv4 or IPv6. IPv4 addresses are host order in
// `addr`; IPv6 addresses are network order in `addr6`. AF_INET6 sockets
// talking to IPv4 peers (v4-mapped ::ffff:a.b.c.d) hold IPv4 endpoints.
struct Endpoint {
    u32 addr = 0;
    u16 port = 0;
    bool v6 = false;
    u8 addr6[16] = {};
    u32 scope = 0; // interface index of a link-local IPv6 address

    bool any() const {
        if (!v6)
            return addr == 0;
        for (u8 b : addr6)
            if (b)
                return false;
        return true;
    }
    bool same_addr(const Endpoint& o) const {
        if (v6 != o.v6)
            return false;
        return v6 ? memcmp(addr6, o.addr6, 16) == 0 : addr == o.addr;
    }
};

inline Endpoint ep4(u32 addr, u16 port = 0) { return Endpoint{addr, port}; }
inline Endpoint ep6(const u8* a, u16 port = 0, u32 scope = 0) {
    Endpoint e;
    e.v6 = true;
    memcpy(e.addr6, a, 16);
    e.port = port;
    e.scope = scope;
    return e;
}

// Internet checksum helpers.
u32 checksum_add(u32 sum, const void* data, usize len);
u16 checksum_finish(u32 sum);
u16 pseudo_checksum(u32 src, u32 dst, u8 proto, const u8* segment, usize len);
u16 pseudo_checksum6(const u8* src, const u8* dst, u8 proto, const u8* segment, usize len);
// TCP/UDP checksum over the right pseudo header for the endpoints' family.
u16 l4_checksum(const Endpoint& src, const Endpoint& dst, u8 proto, const u8* segment, usize len);

// IPv4 output: builds the header, routes, resolves the next hop and
// fragments when needed. `src` 0 picks the outgoing interface's address. Lock held.
long ip_send(u32 src, u32 dst, u8 proto, const u8* payload, usize len, u8 ttl = 64, Interface* via = nullptr);
// The source address the stack would use to reach `dst` (0 if unroutable).
u32 source_for(u32 dst);
bool is_local_address(u32 addr);

// Family-neutral output for TCP/UDP: sends `seg` from src to dst (lock held).
long ip_output(const Endpoint& src, const Endpoint& dst, u8 proto, const u8* seg, usize len);
// The local address to use towards `dst` (false: no route).
bool source_endpoint(const Endpoint& dst, Endpoint* src);
bool is_local_endpoint(const Endpoint& e);
// Largest TCP segment payload towards `dst` without fragmenting.
u32 path_mss(const Endpoint& dst);

// Ethernet output (core.cpp) and the loopback interface.
void eth_send(Interface* iface, const u8* dst_mac, u16 type, const u8* payload, usize len);
Interface* loopback_interface();
constexpr u16 ETH_IPV6 = 0x86DD;

// ---- IPv6 (ipv6.cpp) ----
namespace ip6 {
inline bool is_unspecified(const u8* a) {
    for (int i = 0; i < 16; i++)
        if (a[i])
            return false;
    return true;
}
inline bool is_loopback(const u8* a) {
    for (int i = 0; i < 15; i++)
        if (a[i])
            return false;
    return a[15] == 1;
}
inline bool is_link_local(const u8* a) { return a[0] == 0xFE && (a[1] & 0xC0) == 0x80; }
inline bool is_multicast(const u8* a) { return a[0] == 0xFF; }
inline bool is_v4mapped(const u8* a) {
    for (int i = 0; i < 10; i++)
        if (a[i])
            return false;
    return a[10] == 0xFF && a[11] == 0xFF;
}
} // namespace ip6

// Send an IPv6 packet. `src` null picks a source address. Lock held.
long ip6_send(const u8* src, const u8* dst, u32 scope, u8 proto, const u8* payload, usize len, u8 hop_limit = 64);
bool source6_for(const u8* dst, u32 scope, u8* out);
bool is_local_address6(const u8* a);
void ip6_input(Interface* in, const u8* pkt, usize len);
void ipv6_init_interface(Interface* iface); // link-local address, router solicitation
void ipv6_timer(u64 now_ms);
void icmp6_input(Interface* in, const u8* ip6, const u8* data, usize len);
// ICMPv6 destination unreachable about the packet at `ip6` (e.g. a closed UDP port).
void icmp6_unreachable(const u8* ip6, usize len, u8 code);
Socket* create_icmp6(int type);

// Interface IPv6 configuration, for netlink and /proc/net/if_inet6.
struct Addr6 {
    u8 addr[16];
    u8 prefix_len;
    u8 scope;    // RT_SCOPE_*: 0 global, 253 link, 254 host
    u8 flags;    // IFA_F_*
    bool dynamic; // from SLAAC
    u32 ifindex;
};
void for_each_addr6(void (*fn)(const Addr6&, void*), void* ctx);
struct Route6 {
    u8 dst[16];
    u8 dst_len;
    u8 gateway[16];
    bool has_gateway;
    u32 ifindex;
    u8 protocol; // RTPROT_*
};
void for_each_route6(void (*fn)(const Route6&, void*), void* ctx);
long add_addr6(u32 ifindex, const u8* addr, u8 prefix_len);
long del_addr6(u32 ifindex, const u8* addr);
long set_default_route6(const u8* gateway, u32 ifindex); // null gateway: remove
bool dns_server6(u8* out);

// ---- fragments (frag.cpp) ----
// Feed one fragment; when the datagram is complete returns a heap buffer
// holding it (caller kfree()s) with its length in *total. `key` identifies
// the datagram (addresses, id, protocol).
struct FragKey {
    bool v6;
    u8 src[16], dst[16];
    u32 id;
    u8 proto;
};
u8* frag_add(const FragKey& key, usize offset, bool more, const u8* data, usize len, usize* total);
void frag_timer(u64 now_ms);

// Protocol input (lock held). `ip` points at the IPv4 or IPv6 header (for
// ICMP error replies); src/dst carry the packet's addresses.
void icmp_input(Interface* in, const u8* ip, usize ip_len, const u8* data, usize len);
void udp_input(Interface* in, const u8* ip, usize ip_len, const Endpoint& src, const Endpoint& dst, const u8* data,
               usize len);
void tcp_input(Interface* in, const Endpoint& src, const Endpoint& dst, const u8* data, usize len);
void tcp_timer(u64 now_ms); // retransmissions, TIME_WAIT (lock held)

// DHCP client hook: UDP datagrams to port 68 go here.
void dhcp_input(Interface* in, const u8* data, usize len);
void dhcp_start(Interface* iface);
void dhcp_kick(Interface* iface);
bool dhcp_configured();
u32 dns_server();
void set_gateway(u32 gw);

// ---- sockets ----

// sockaddr_in as programs see it.
struct SockAddrIn {
    u16 family;
    u16 port;    // network order
    u32 addr;    // network order
    u8 zero[8];
};
static_assert(sizeof(SockAddrIn) == 16);
// sockaddr_in6.
struct SockAddrIn6 {
    u16 family;
    u16 port;     // network order
    u32 flowinfo;
    u8 addr[16];
    u32 scope_id;
};
static_assert(sizeof(SockAddrIn6) == 28);
constexpr int SOCK_STREAM = 1, SOCK_DGRAM = 2, SOCK_RAW = 3;
constexpr int MSG_PEEK = 0x2, MSG_DONTWAIT = 0x40, MSG_WAITALL = 0x100, MSG_NOSIGNAL = 0x4000, MSG_TRUNC = 0x20;


constexpr u16 AF_UNIX = 1, AF_NETLINK = 16;
constexpr int SOCK_SEQPACKET = 5;
constexpr int MSG_CTRUNC = 0x8, MSG_EOR = 0x80, MSG_CMSG_CLOEXEC = 0x40000000;
constexpr long EPROTOTYPE = 91, ENOPROTOOPT = 92, EAFNOSUPPORT = 97;

// A socket address exactly as programs pass it (struct sockaddr_storage).
// Families other than AF_INET (Unix paths, netlink port ids) are
// interpreted by the socket itself.
struct SockAddr {
    u32 len = 0;
    u8 data[128] = {};
    u16 family() const { return len >= 2 ? static_cast<u16>(data[0] | (data[1] << 8)) : AF_UNSPEC; }
};

// struct ucred (SCM_CREDENTIALS, SO_PEERCRED).
struct Ucred {
    i32 pid = 0;
    u32 uid = 0, gid = 0;
};

// Ancillary data of sendmsg/recvmsg (control messages); only AF_UNIX
// sockets carry it. `files` is a heap array of file references owned by
// whoever holds the Control; release_control() drops them.
struct Control {
    vfs::File** files = nullptr; // SCM_RIGHTS
    int nfiles = 0;
    bool has_cred = false;       // SCM_CREDENTIALS
    Ucred cred;
};
void release_control(Control& c);
Ucred current_cred();

// Socket option hooks return this when they don't handle an option, so the
// generic SOL_SOCKET code runs instead.
constexpr long OPT_UNHANDLED = 1;

class Socket : public vfs::Inode {
public:
    Socket(int type, int protocol, u16 domain = AF_INET);
    ~Socket() override = default;

    int type, protocol;
    u16 domain;
    Endpoint local, remote;
    bool bound = false, connected = false;
    int error = 0;          // pending error (SO_ERROR), positive errno
    bool reuse_addr = false;
    bool v6only = false;    // IPV6_V6ONLY (AF_INET6 sockets)
    bool nonblock_default = false;
    u64 rcv_timeout_ns = 0, snd_timeout_ns = 0; // 0 = forever
    sched::WaitQueue wq;    // readers, writers, connect/accept waiters

    // Operations, in kernel memory. `flags` are MSG_*; `nonblock` from the file.
    virtual long bind(const Endpoint& ep);
    virtual long connect(const Endpoint& ep, bool nonblock);
    virtual long listen(int backlog);
    virtual long accept(Socket** out, bool nonblock);
    virtual isize send(const u8* data, usize len, int flags, const Endpoint* to, bool nonblock) = 0;
    // `from` is filled for datagram sockets; returns bytes (or the full
    // datagram length with MSG_TRUNC semantics handled by the caller).
    virtual isize recv(u8* buf, usize len, int flags, Endpoint* from, bool nonblock, usize* full_len) = 0;
    virtual long shutdown(int how);
    virtual u16 poll_mask() = 0;
    virtual void close() = 0; // the last file reference is gone
    virtual usize bytes_available() { return 0; }

    // Address-family-neutral entry points used by the system calls. The
    // defaults translate sockaddr_in to Endpoint and call the operations
    // above; Unix and netlink sockets override them.
    virtual long bind_addr(const SockAddr& sa);
    virtual long connect_addr(const SockAddr& sa, bool nonblock);
    virtual long get_name(SockAddr* out, bool peer); // getsockname / getpeername
    // `ctl` (may be null) carries control messages; a socket that keeps the
    // passed files takes them out of `ctl` (sets nfiles to 0).
    virtual isize send_msg(const u8* data, usize len, int flags, const SockAddr* to, Control* ctl, bool nonblock);
    virtual isize recv_msg(u8* buf, usize len, int flags, SockAddr* from, Control* ctl, bool nonblock,
                           usize* full_len);
    // Per-family socket options; return OPT_UNHANDLED to fall back to the generic code.
    virtual long set_option(int level, int name, const u8* val, u32 len) { return OPT_UNHANDLED; }
    virtual long get_option(int level, int name, u8* val, u32* len) { return OPT_UNHANDLED; }

    // vfs::Inode plumbing
    bool seekable() const override { return false; }
    isize read_file(vfs::File* f, u64, void* buf, usize count) override;
    isize write_file(vfs::File* f, u64, const void* buf, usize count) override;
    u16 poll(vfs::File*) override { return poll_mask(); }
    void on_release(vfs::File*) override;
    long ioctl(u64 request, u64 arg) override;

    void wake();
};

// Block (stack lock NOT held) until cond() holds. Returns 0, -EAGAIN when
// non-blocking or timed out, -EINTR on a signal.
template <typename Cond>
long sock_wait(Socket* s, bool nonblock, u64 timeout_ns, Cond cond) {
    if (cond())
        return 0;
    if (nonblock)
        return -err::AGAIN;
    bool intr = false;
    auto done = [&] {
        if (cond())
            return true;
        if (signal::interrupted()) {
            intr = true;
            return true;
        }
        return false;
    };
    if (timeout_ns) {
        if (!s->wq.wait_until_timeout(done, timeout_ns))
            return -err::AGAIN;
    } else {
        s->wq.wait_until(done);
    }
    return intr && !cond() ? -err::INTR : 0;
}

// sockaddr_in / sockaddr_in6 <-> Endpoint for a socket of family `domain`:
// AF_INET sockets take sockaddr_in only; AF_INET6 sockets take
// sockaddr_in6 and turn v4-mapped addresses into IPv4 endpoints (and
// report IPv4 endpoints as v4-mapped).
long sockaddr_to_endpoint(const SockAddr& sa, Endpoint* out, u16 domain = AF_INET);
void endpoint_to_sockaddr(const Endpoint& ep, SockAddr* out, u16 domain = AF_INET);

// Would a socket bound to `s->local` receive a packet sent to `dst`?
// (wildcards, IPv4 vs IPv6, IPV6_V6ONLY)
bool local_accepts(const Socket* s, const Endpoint& dst);
// Do two local bindings overlap (for EADDRINUSE)?
bool bindings_overlap(const Endpoint& a, const Endpoint& b);

Socket* create_unix(int type);
long unix_socketpair(int type, Socket** a, Socket** b);
Socket* create_netlink(int type, int protocol);

Socket* create_udp(int type, int protocol, u16 domain = AF_INET);
Socket* create_tcp(u16 domain = AF_INET);
Socket* create_icmp(int type);

// Port allocation for UDP/TCP (lock held). Returns 0 when exhausted.
u16 ephemeral_port(u8 proto);
bool port_in_use(u8 proto, const Endpoint& addr, u16 port);

// /proc/net/* contents.
void format_ip6(const u8* a, char* out, usize size);
void proc_net_init();
void unix_proc_init(); // /proc/net/unix
void for_each_tcp(void (*fn)(Socket*, int state, void*), void* ctx);
void for_each_udp(void (*fn)(Socket*, void*), void* ctx);

} // namespace net
