// ICMP: answers pings, reports errors to UDP sockets, and provides raw
// ICMP sockets (ping as root) and ping sockets (SOCK_DGRAM, IPPROTO_ICMP).

#include <mm/heap.hpp>
#include <net/internal.hpp>

namespace net {

void udp_icmp_error(u32 dst, u16 sport, u16 dport);

namespace {

struct Packet {
    Endpoint from;
    usize len;
    Packet* next;
    u8 data[];
};

class IcmpSocket;
IcmpSocket* g_icmp = nullptr;
u16 g_next_id = 1;

class IcmpSocket : public Socket {
public:
    explicit IcmpSocket(int t) : Socket(t, PROTO_ICMP) {
        LockGuard guard(g_lock);
        local.port = g_next_id++; // ping sockets: the echo identifier
        next = g_icmp;
        g_icmp = this;
    }

    IcmpSocket* next = nullptr;
    Packet* head = nullptr;
    Packet* tail = nullptr;
    usize count = 0;

    long bind(const Endpoint& ep) override {
        LockGuard guard(g_lock);
        local.addr = ep.addr;
        if (type == SOCK_DGRAM && ep.port)
            local.port = ep.port;
        bound = true;
        return 0;
    }

    long connect(const Endpoint& ep, bool) override {
        LockGuard guard(g_lock);
        remote = ep;
        connected = ep.addr != 0;
        return 0;
    }

    isize send(const u8* data, usize len, int, const Endpoint* to, bool) override {
        LockGuard guard(g_lock);
        Endpoint dst = to ? *to : remote;
        if (!to && !connected)
            return -EDESTADDRREQ;
        if (len < 8 || len > 1480)
            return len < 8 ? -err::INVAL : -EMSGSIZE;
        u8 msg[1480];
        memcpy(msg, data, len);
        if (type == SOCK_DGRAM) { // the kernel owns the identifier and checksum
            put16(msg + 4, local.port);
            put16(msg + 2, 0);
            put16(msg + 2, checksum_finish(checksum_add(0, msg, len)));
        }
        long r = ip_send(local.addr, dst.addr, PROTO_ICMP, msg, len);
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

    void deliver(u32 src, const u8* data, usize len) {
        if (count >= 64)
            return;
        auto* p = static_cast<Packet*>(kmalloc(sizeof(Packet) + len));
        if (!p)
            return;
        p->from = Endpoint{src, 0};
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
            IcmpSocket** link = &g_icmp;
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

Socket* create_icmp(int type) { return new IcmpSocket(type); }

void icmp_input(Interface* in, const u8* ip, usize ip_len, const u8* data, usize len) {
    if (len < 8 || checksum_finish(checksum_add(0, data, len)) != 0)
        return;
    u32 src = get32(ip + 12), dst = get32(ip + 16);
    u8 type = data[0];

    if (type == 8) { // echo request: answer with the same payload
        if (len > 1480)
            return;
        u8 reply[1480];
        memcpy(reply, data, len);
        reply[0] = 0;
        put16(reply + 2, 0);
        put16(reply + 2, checksum_finish(checksum_add(0, reply, len)));
        u32 from = is_local_address(dst) ? dst : in->addr;
        ip_send(from, src, PROTO_ICMP, reply, len);
    } else if (type == 3 && len >= 8 + 20 + 8) { // destination unreachable: tell UDP
        const u8* orig = data + 8;
        usize ohl = (orig[0] & 0xF) * 4u;
        if (orig[9] == PROTO_UDP && len >= 8 + ohl + 8)
            udp_icmp_error(get32(orig + 16), get16(orig + ohl), get16(orig + ohl + 2));
    }

    for (IcmpSocket* s = g_icmp; s; s = s->next) {
        if (s->type == SOCK_RAW) {
            s->deliver(src, ip, ip_len); // raw sockets see the IP header too
        } else if (type == 0 && get16(data + 4) == s->local.port) {
            s->deliver(src, data, len);
        }
    }
}

} // namespace net
