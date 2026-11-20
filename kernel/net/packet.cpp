// AF_PACKET sockets (Linux packet(7)): raw Ethernet frames of one type (or
// all) on one interface (or any). wpa_supplicant exchanges its WPA2 keys
// (EAPOL, 0x888e) through them; DHCP clients and tcpdump use them too.
// SOCK_RAW carries whole frames, SOCK_DGRAM the payload plus a sockaddr_ll.

#include <lib/errno.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <net/internal.hpp>
#include <net/net.hpp>
#include <proc/process.hpp>

namespace net {

namespace {

constexpr u16 AF_PACKET = 17;
constexpr u16 ETH_P_ALL = 0x0003;
constexpr int SOL_PACKET = 263, SO_ATTACH_FILTER = 26, SO_DETACH_FILTER = 27, SO_LOCK_FILTER = 44;
constexpr usize QUEUE_LIMIT = 256 * 1024;

struct SockAddrLl { // struct sockaddr_ll
    u16 family;
    u16 protocol; // network order
    i32 ifindex;
    u16 hatype;
    u8 pkttype;
    u8 halen;
    u8 addr[8];
};
static_assert(sizeof(SockAddrLl) == 20);

u16 swap16(u16 v) { return static_cast<u16>((v >> 8) | (v << 8)); }

struct Frame {
    Frame* next;
    u32 ifindex;
    u16 proto;     // host order
    u8 src[6];
    u8 pkttype;    // 0 host, 1 broadcast, 2 multicast, 3 other host
    usize len;
    u8 data[];
};

class PacketSocket;
sched::Mutex g_pk_lock;
PacketSocket* g_sockets = nullptr;

class PacketSocket : public Socket {
public:
    PacketSocket(int t, u16 p) : Socket(t, p, AF_PACKET), proto(p) {}
    PacketSocket* next = nullptr;
    u16 proto;        // host order; 0 = nothing until bound
    u32 ifindex = 0;  // 0 = every interface
    sched::Mutex qlock;
    Frame* head = nullptr;
    Frame** tail = &head;
    usize queued = 0;

    void deliver(Interface* iface, const u8* frame, usize len) {
        usize payload = type == SOCK_RAW ? len : len - 14;
        if (queued + payload > QUEUE_LIMIT)
            return; // full, like a socket buffer: drop
        auto* f = static_cast<Frame*>(kmalloc(sizeof(Frame) + payload));
        if (!f)
            return;
        f->next = nullptr;
        f->ifindex = iface->index;
        f->proto = static_cast<u16>((frame[12] << 8) | frame[13]);
        memcpy(f->src, frame + 6, 6);
        bool group = frame[0] & 1;
        bool bcast = group && memcmp(frame, "\xff\xff\xff\xff\xff\xff", 6) == 0;
        f->pkttype = bcast ? 1 : group ? 2 : memcmp(frame, iface->mac, 6) == 0 ? 0 : 3;
        f->len = payload;
        memcpy(f->data, type == SOCK_RAW ? frame : frame + 14, payload);
        {
            LockGuard guard(qlock);
            *tail = f;
            tail = &f->next;
            queued += payload;
        }
        wake();
    }

    long bind_addr(const SockAddr& sa) override {
        if (sa.len < 12 || sa.family() != AF_PACKET)
            return -err::INVAL;
        SockAddrLl a{};
        memcpy(&a, sa.data, min<usize>(sa.len, sizeof(a)));
        if (a.ifindex && !interface_by_index(static_cast<u32>(a.ifindex)))
            return -err::NODEV;
        LockGuard guard(g_pk_lock);
        ifindex = static_cast<u32>(a.ifindex);
        if (a.protocol)
            proto = swap16(a.protocol);
        return 0;
    }

    long get_name(SockAddr* out, bool peer) override {
        if (peer)
            return -err::OPNOTSUPP;
        SockAddrLl a{};
        a.family = AF_PACKET;
        a.protocol = swap16(proto);
        a.ifindex = static_cast<i32>(ifindex);
        a.hatype = 1; // ARPHRD_ETHER
        a.halen = 6;
        if (Interface* i = ifindex ? interface_by_index(ifindex) : nullptr)
            memcpy(a.addr, i->mac, 6);
        *out = SockAddr{};
        memcpy(out->data, &a, sizeof(a));
        out->len = sizeof(a);
        return 0;
    }

    isize send_msg(const u8* data, usize len, int, const SockAddr* to, Control*, bool) override {
        SockAddrLl a{};
        if (to)
            memcpy(&a, to->data, min<usize>(to->len, sizeof(a)));
        u32 idx = to && a.ifindex ? static_cast<u32>(a.ifindex) : ifindex;
        Interface* iface = idx ? interface_by_index(idx) : nullptr;
        if (!iface)
            return -err::NXIO;
        if (type == SOCK_RAW) {
            if (len < 14)
                return -err::INVAL;
            eth_send(iface, data, static_cast<u16>((data[12] << 8) | data[13]), data + 14, len - 14);
        } else {
            if (!to || a.halen < 6)
                return -EDESTADDRREQ;
            u16 p = a.protocol ? swap16(a.protocol) : proto;
            eth_send(iface, a.addr, p, data, len);
        }
        return static_cast<isize>(len);
    }

    isize recv_msg(u8* buf, usize len, int flags, SockAddr* from, Control*, bool nonblock,
                   usize* full_len) override {
        for (;;) {
            if (long r = sock_wait(this, nonblock, rcv_timeout_ns, [&] { return head != nullptr; }); r < 0)
                return r;
            Frame* f;
            {
                LockGuard guard(qlock);
                f = head;
                if (!f)
                    continue;
                if (!(flags & MSG_PEEK)) {
                    head = f->next;
                    if (!head)
                        tail = &head;
                    queued -= f->len;
                }
            }
            usize n = min(len, f->len);
            memcpy(buf, f->data, n);
            if (full_len)
                *full_len = f->len;
            if (from) {
                SockAddrLl a{};
                a.family = AF_PACKET;
                a.protocol = swap16(f->proto);
                a.ifindex = static_cast<i32>(f->ifindex);
                a.hatype = 1;
                a.pkttype = f->pkttype;
                a.halen = 6;
                memcpy(a.addr, f->src, 6);
                *from = SockAddr{};
                memcpy(from->data, &a, sizeof(a));
                from->len = sizeof(a);
            }
            if (!(flags & MSG_PEEK))
                kfree(f);
            return static_cast<isize>(n);
        }
    }

    isize send(const u8* data, usize len, int flags, const Endpoint*, bool nonblock) override {
        return send_msg(data, len, flags, nullptr, nullptr, nonblock);
    }
    isize recv(u8* buf, usize len, int flags, Endpoint*, bool nonblock, usize* full_len) override {
        return recv_msg(buf, len, flags, nullptr, nullptr, nonblock, full_len);
    }

    // Socket filters and memberships: accepted (frames are already filtered by type).
    long set_option(int level, int name, const u8*, u32) override {
        if (level == SOL_PACKET)
            return 0;
        if (level == 1 /* SOL_SOCKET */ && (name == SO_ATTACH_FILTER || name == SO_DETACH_FILTER || name == SO_LOCK_FILTER))
            return 0;
        return OPT_UNHANDLED;
    }

    u16 poll_mask() override { return ::poll::POLLOUT | (head ? ::poll::POLLIN : 0); }
    usize bytes_available() override {
        LockGuard guard(qlock);
        return head ? head->len : 0;
    }

    void close() override {
        {
            LockGuard guard(g_pk_lock);
            PacketSocket** link = &g_sockets;
            while (*link && *link != this)
                link = &(*link)->next;
            if (*link)
                *link = next;
        }
        while (head) {
            Frame* f = head;
            head = f->next;
            kfree(f);
        }
    }
};

} // namespace

Socket* create_packet(int type, int protocol_be) {
    auto* s = new PacketSocket(type, swap16(static_cast<u16>(protocol_be)));
    LockGuard guard(g_pk_lock);
    s->next = g_sockets;
    g_sockets = s;
    return s;
}

void packet_deliver(Interface* iface, const u8* frame, usize len) {
    if (!__atomic_load_n(&g_sockets, __ATOMIC_ACQUIRE) || len < 14)
        return;
    u16 type = static_cast<u16>((frame[12] << 8) | frame[13]);
    LockGuard guard(g_pk_lock);
    for (PacketSocket* s = g_sockets; s; s = s->next)
        if ((s->proto == ETH_P_ALL || s->proto == type) && s->proto && (!s->ifindex || s->ifindex == iface->index))
            s->deliver(iface, frame, len);
}

} // namespace net
