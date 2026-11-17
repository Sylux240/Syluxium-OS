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

constexpr u8 PROTO_ICMP = 1, PROTO_TCP = 6, PROTO_UDP = 17;
constexpr u32 LOOPBACK_ADDR = 0x7F000001;
constexpr usize MAX_FRAME = 1514;

// One lock for the whole stack: protocol state, sockets, ARP. Held while
// processing a received packet or a socket call; never across sleeps.
extern sched::Mutex g_lock;

// Internet checksum helpers.
u32 checksum_add(u32 sum, const void* data, usize len);
u16 checksum_finish(u32 sum);
u16 pseudo_checksum(u32 src, u32 dst, u8 proto, const u8* segment, usize len);

// IPv4 output: builds the header, routes, resolves the next hop. `src` 0
// picks the outgoing interface's address. Lock held.
long ip_send(u32 src, u32 dst, u8 proto, const u8* payload, usize len, u8 ttl = 64);
// The source address the stack would use to reach `dst` (0 if unroutable).
u32 source_for(u32 dst);
bool is_local_address(u32 addr);

// Protocol input (lock held). `ip` points at the IPv4 header.
void icmp_input(Interface* in, const u8* ip, usize ip_len, const u8* data, usize len);
void udp_input(Interface* in, const u8* ip, const u8* data, usize len);
void tcp_input(Interface* in, const u8* ip, const u8* data, usize len);
void tcp_timer(u64 now_ms); // retransmissions, TIME_WAIT (lock held)

// DHCP client hook: UDP datagrams to port 68 go here.
void dhcp_input(Interface* in, const u8* data, usize len);
void dhcp_start(Interface* iface);
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
constexpr u16 AF_UNSPEC = 0, AF_INET = 2;
constexpr int SOCK_STREAM = 1, SOCK_DGRAM = 2, SOCK_RAW = 3;
constexpr int MSG_PEEK = 0x2, MSG_DONTWAIT = 0x40, MSG_WAITALL = 0x100, MSG_NOSIGNAL = 0x4000, MSG_TRUNC = 0x20;

// Endpoint in host order.
struct Endpoint {
    u32 addr = 0;
    u16 port = 0;
};

class Socket : public vfs::Inode {
public:
    Socket(int type, int protocol);
    ~Socket() override = default;

    int type, protocol;
    Endpoint local, remote;
    bool bound = false, connected = false;
    int error = 0;          // pending error (SO_ERROR), positive errno
    bool reuse_addr = false;
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

Socket* create_udp(int type, int protocol);
Socket* create_tcp();
Socket* create_icmp(int type);

// Port allocation for UDP/TCP (lock held). Returns 0 when exhausted.
u16 ephemeral_port(u8 proto);
bool port_in_use(u8 proto, u32 addr, u16 port);

// /proc/net/* contents.
void proc_net_init();
void for_each_tcp(void (*fn)(Socket*, int state, void*), void* ctx);
void for_each_udp(void (*fn)(Socket*, void*), void* ctx);

} // namespace net
