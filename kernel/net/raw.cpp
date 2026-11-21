// Raw IPv4 sockets (SOCK_RAW, root only), other than ICMP (icmp.cpp):
// socket(AF_INET, SOCK_RAW, proto) receives a copy of every IPv4 packet of
// that protocol, header included, and sends payloads the kernel puts an IP
// header on; IPPROTO_RAW sockets (and IP_HDRINCL) send whole packets and
// receive nothing. busybox udhcpc uses one for interface ioctls and its
// raw DHCP packets.

#include <mm/heap.hpp>
#include <net/internal.hpp>

namespace net {

namespace {

constexpr int IPPROTO_RAW_ = 255;
constexpr int IP_HDRINCL = 3;
constexpr usize MAX_QUEUED = 256 * 1024;

struct Packet {
    Packet* next;
    u32 src;
    usize len;
    u8 data[];
};

class RawSocket;
RawSocket* g_raw = nullptr; // lock held

class RawSocket : public Socket {
public:
    explicit RawSocket(int proto) : Socket(SOCK_RAW, proto, AF_INET), hdrincl(proto == IPPROTO_RAW_) {
        LockGuard guard(g_lock);
        next = g_raw;
        g_raw = this;
    }

    RawSocket* next = nullptr;
    bool hdrincl;
    Packet* head = nullptr;
    Packet** tail = &head;
    usize queued = 0;

    long bind(const Endpoint& ep) override {
        local = ep;
        bound = true;
        return 0;
    }

    long connect(const Endpoint& ep, bool) override {
        remote = ep;
        connected = !ep.any();
        return 0;
    }

    isize send(const u8* data, usize len, int, const Endpoint* to, bool) override {
        LockGuard guard(g_lock);
        if (hdrincl) {
            long r = ip_send_raw(data, len);
            return r < 0 ? r : static_cast<isize>(len);
        }
        Endpoint dst = to ? *to : remote;
        if (dst.v6 || dst.any())
            return -EDESTADDRREQ;
        long r = ip_send(local.any() ? 0 : local.addr, dst.addr, static_cast<u8>(protocol), data, len);
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
                *from = ep4(p->src);
            if (full_len)
                *full_len = p->len;
            if (!(flags & MSG_PEEK)) {
                head = p->next;
                if (!head)
                    tail = &head;
                queued -= p->len;
                kfree(p);
            }
            return static_cast<isize>(n);
        }
    }

    long set_option(int level, int name, const u8* val, u32 len) override {
        if (level == 0 && name == IP_HDRINCL && len >= 4) {
            i32 v;
            memcpy(&v, val, 4);
            hdrincl = v != 0 || protocol == IPPROTO_RAW_;
            return 0;
        }
        return OPT_UNHANDLED;
    }

    u16 poll_mask() override { return ::poll::POLLOUT | (head ? ::poll::POLLIN : 0); }
    usize bytes_available() override { return head ? head->len : 0; }

    void queue(const u8* ip, usize len) {
        if (queued + len > MAX_QUEUED)
            return;
        auto* p = static_cast<Packet*>(kmalloc(sizeof(Packet) + len));
        if (!p)
            return;
        p->next = nullptr;
        p->src = get32(ip + 12);
        p->len = len;
        memcpy(p->data, ip, len);
        *tail = p;
        tail = &p->next;
        queued += len;
        wake();
    }

    void close() override {
        {
            LockGuard guard(g_lock);
            RawSocket** link = &g_raw;
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

} // namespace

Socket* create_raw(int protocol) { return new RawSocket(protocol); }

void raw_deliver(Interface*, const u8* ip, usize len) {
    for (RawSocket* s = g_raw; s; s = s->next) {
        if (s->protocol == IPPROTO_RAW_ || s->protocol != ip[9])
            continue;
        if (s->bound && !s->local.any() && s->local.addr != get32(ip + 16))
            continue;
        if (s->connected && s->remote.addr != get32(ip + 12))
            continue;
        s->queue(ip, len);
    }
}

} // namespace net
