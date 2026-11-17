// TCP (RFC 793/9293 essentials): connection setup and teardown, in-order
// delivery, retransmission with an RTT-based timeout (RFC 6298), flow
// control (receive window), simple congestion control (slow start +
// reset on timeout), listen/accept, half-close.
//
// Not yet: window scaling, SACK, out-of-order queueing (such segments are
// dropped and the peer retransmits), fast retransmit, keep-alives.

#include <core/log.hpp>
#include <mm/heap.hpp>
#include <net/internal.hpp>
#include <time/clock.hpp>

namespace net {

bool port_in_use_udp(u32 addr, u16 port);

namespace {

enum class State : u8 {
    Closed, Listen, SynSent, SynReceived, Established, FinWait1, FinWait2, CloseWait, Closing, LastAck, TimeWait,
};

// /proc/net/tcp numbering (Linux's TCP_* states).
int linux_state(State s) {
    switch (s) {
    case State::Established: return 1;
    case State::SynSent: return 2;
    case State::SynReceived: return 3;
    case State::FinWait1: return 4;
    case State::FinWait2: return 5;
    case State::TimeWait: return 6;
    case State::Closed: return 7;
    case State::CloseWait: return 8;
    case State::LastAck: return 9;
    case State::Listen: return 10;
    case State::Closing: return 11;
    }
    return 7;
}

constexpr u8 FIN = 0x01, SYN = 0x02, RST = 0x04, PSH = 0x08, ACK = 0x10;
constexpr usize BUF = 64 * 1024;
constexpr u32 OUR_MSS = 1460;
constexpr u64 TIME_WAIT_MS = 4000;
constexpr u64 FIN_WAIT2_ORPHAN_MS = 60000;

bool seq_lt(u32 a, u32 b) { return static_cast<i32>(a - b) < 0; }
bool seq_le(u32 a, u32 b) { return static_cast<i32>(a - b) <= 0; }
bool seq_gt(u32 a, u32 b) { return static_cast<i32>(a - b) > 0; }

u32 random_iss() {
    static u32 counter = 0x5EED;
    u64 t = clock::now_ns();
    counter = counter * 1103515245u + 12345u;
    return static_cast<u32>(t ^ (t >> 29)) ^ counter;
}

// Byte ring buffer.
struct Ring {
    u8* data = nullptr;
    usize head = 0, len = 0;
    usize free() const { return BUF - len; }
    void push(const u8* src, usize n) {
        for (usize i = 0; i < n; i++)
            data[(head + len + i) % BUF] = src[i];
        len += n;
    }
    void peek(usize offset, u8* dst, usize n) const {
        for (usize i = 0; i < n; i++)
            dst[i] = data[(head + offset + i) % BUF];
    }
    void drop(usize n) {
        head = (head + n) % BUF;
        len -= n;
    }
};

class TcpSocket;
TcpSocket* g_tcp = nullptr;

class TcpSocket : public Socket {
public:
    TcpSocket() : Socket(SOCK_STREAM, PROTO_TCP) {
        sbuf.data = static_cast<u8*>(kmalloc(BUF));
        rbuf.data = static_cast<u8*>(kmalloc(BUF));
        next = g_tcp;
        g_tcp = this; // caller holds g_lock
    }
    ~TcpSocket() override {
        kfree(sbuf.data);
        kfree(rbuf.data);
    }

    TcpSocket* next = nullptr;
    State state = State::Closed;
    bool owned = true;               // a file refers to it
    TcpSocket* parent = nullptr;     // listener (for not-yet-accepted connections)
    bool in_accept_queue = false;
    TcpSocket* accept_next = nullptr;
    TcpSocket* accept_head = nullptr; // listener: established, not yet accepted
    int backlog = 0, queued = 0;

    // Send side.
    Ring sbuf;
    u32 iss = 0, snd_una = 0, snd_nxt = 0, sbuf_seq = 0; // sbuf_seq: sequence number of sbuf[0]
    u32 snd_wnd = 0, cwnd = 0, mss = 536;
    bool fin_queued = false, fin_sent = false;
    u32 fin_seq = 0;
    bool wr_shutdown = false;
    // Retransmission.
    u64 rtx_deadline = 0;
    u32 rto_ms = 1000, srtt = 0, rttvar = 0;
    int retries = 0;
    bool rtt_timing = false;
    u32 rtt_seq = 0;
    u64 rtt_start = 0;
    // Receive side.
    Ring rbuf;
    u32 irs = 0, rcv_nxt = 0;
    bool fin_received = false, rd_shutdown = false;
    u32 last_window = BUF;
    u64 state_since = 0;

    bool ok() const { return sbuf.data && rbuf.data; }

    u16 window() const { return static_cast<u16>(min<usize>(rbuf.free(), 65535)); }

    void set_state(State s) {
        state = s;
        state_since = clock::now_ms();
        wake();
        if (parent)
            parent->wake();
    }

    // Build and send one segment. `data_offset` indexes sbuf.
    void send_segment(u32 seq, u8 flags, usize data_offset, usize len) {
        u8 seg[20 + 4 + OUR_MSS];
        usize hl = (flags & SYN) ? 24 : 20;
        put16(seg, local.port);
        put16(seg + 2, remote.port);
        put32(seg + 4, seq);
        put32(seg + 8, (flags & ACK) ? rcv_nxt : 0);
        seg[12] = static_cast<u8>((hl / 4) << 4);
        seg[13] = flags;
        u16 w = window();
        put16(seg + 14, w);
        put16(seg + 16, 0);
        put16(seg + 18, 0);
        if (flags & SYN) { // MSS option
            seg[20] = 2;
            seg[21] = 4;
            put16(seg + 22, OUR_MSS);
        }
        if (len)
            sbuf.peek(data_offset, seg + hl, len);
        put16(seg + 16, pseudo_checksum(local.addr, remote.addr, PROTO_TCP, seg, hl + len));
        ip_send(local.addr, remote.addr, PROTO_TCP, seg, hl + len);
        if (flags & ACK)
            last_window = w;
    }

    void send_ack() { send_segment(snd_nxt, ACK, 0, 0); }

    void arm_timer() {
        if (!rtx_deadline)
            rtx_deadline = clock::now_ms() + rto_ms;
    }

    // Send whatever the windows allow.
    void output(bool probe = false) {
        if (state != State::Established && state != State::CloseWait && state != State::FinWait1 &&
            state != State::Closing && state != State::LastAck)
            return;
        for (;;) {
            u32 offset = snd_nxt - sbuf_seq;
            if (offset < sbuf.len) {
                u32 inflight = snd_nxt - snd_una;
                u32 wnd = min(snd_wnd, cwnd);
                if (probe && wnd == 0)
                    wnd = 1; // zero-window probe
                if (inflight >= wnd)
                    break;
                usize n = min<usize>(min<usize>(sbuf.len - offset, mss), wnd - inflight);
                bool last = offset + n == sbuf.len;
                send_segment(snd_nxt, ACK | (last ? PSH : 0), offset, n);
                if (!rtt_timing) {
                    rtt_timing = true;
                    rtt_seq = snd_nxt;
                    rtt_start = clock::now_ms();
                }
                snd_nxt += static_cast<u32>(n);
                arm_timer();
                probe = false;
            } else if (fin_queued && !fin_sent && offset == sbuf.len) {
                fin_seq = snd_nxt;
                send_segment(snd_nxt, FIN | ACK, 0, 0);
                snd_nxt++;
                fin_sent = true;
                arm_timer();
                break;
            } else {
                break;
            }
        }
        if (snd_wnd == 0 && sbuf.len > snd_nxt - sbuf_seq)
            arm_timer(); // persist: probe the closed window later
    }

    void on_timeout() {
        rtx_deadline = 0;
        int limit = (state == State::SynSent || state == State::SynReceived) ? 6 : 12;
        if (++retries > limit) {
            abort(static_cast<int>(ETIMEDOUT_));
            return;
        }
        rto_ms = min<u32>(rto_ms * 2, 60000);
        rtt_timing = false; // Karn: don't time retransmissions
        switch (state) {
        case State::SynSent:
            send_segment(iss, SYN, 0, 0);
            arm_timer();
            return;
        case State::SynReceived:
            send_segment(iss, SYN | ACK, 0, 0);
            arm_timer();
            return;
        default:
            break;
        }
        // Go back to the oldest unacknowledged byte.
        bool window_probe = snd_wnd == 0;
        snd_nxt = snd_una;
        if (fin_sent && seq_le(snd_una, fin_seq))
            fin_sent = false;
        cwnd = mss;
        output(window_probe);
    }

    // Drop the connection (reset received, timeout, abort on close).
    void abort(int err_code) {
        error = err_code;
        rtx_deadline = 0;
        set_state(State::Closed);
    }

    void send_reset() {
        if (state == State::Closed || state == State::Listen || state == State::SynSent)
            return;
        send_segment(snd_nxt, RST | ACK, 0, 0);
    }

    // ---- socket operations ----

    long bind(const Endpoint& ep) override {
        LockGuard guard(g_lock);
        return bind_locked(ep);
    }

    long bind_locked(const Endpoint& ep) {
        if (bound)
            return -err::INVAL;
        if (ep.addr && !is_local_address(ep.addr))
            return -EADDRNOTAVAIL;
        u16 port = ep.port;
        if (!port) {
            port = ephemeral_port(PROTO_TCP);
            if (!port)
                return -EADDRINUSE;
        } else if (conflicts(ep.addr, port)) {
            return -EADDRINUSE;
        }
        local = Endpoint{ep.addr, port};
        bound = true;
        return 0;
    }

    bool conflicts(u32 addr, u16 port) {
        for (TcpSocket* s = g_tcp; s; s = s->next) {
            if (s == this || !s->bound || s->local.port != port || s->state == State::Closed)
                continue;
            if (s->local.addr && addr && s->local.addr != addr)
                continue;
            if (reuse_addr && s->state != State::Listen)
                continue; // SO_REUSEADDR: TIME_WAIT/old connections don't block a new listener
            return true;
        }
        return false;
    }

    long listen(int n) override {
        LockGuard guard(g_lock);
        if (state != State::Closed && state != State::Listen)
            return -err::INVAL;
        if (!bound)
            if (long r = bind_locked(Endpoint{}); r < 0)
                return r;
        backlog = n < 1 ? 1 : n > 1024 ? 1024 : n;
        set_state(State::Listen);
        return 0;
    }

    long connect(const Endpoint& ep, bool nonblock) override {
        {
            LockGuard guard(g_lock);
            if (state == State::SynSent)
                return -EALREADY;
            if (state == State::Listen)
                return -err::INVAL;
            if (state != State::Closed)
                return -EISCONN;
            if (error) { // a previous attempt failed: report it once, then allow a retry
                error = 0;
            }
            if (!ep.addr || !ep.port)
                return -ECONNREFUSED;
            u32 src = source_for(ep.addr);
            if (!src)
                return -ENETUNREACH;
            if (!bound)
                if (long r = bind_locked(Endpoint{}); r < 0)
                    return r;
            if (!local.addr)
                local.addr = src;
            remote = ep;
            connected = true;
            iss = random_iss();
            snd_una = iss;
            snd_nxt = iss + 1;
            sbuf_seq = iss + 1;
            cwnd = 10 * 536;
            retries = 0;
            rto_ms = 1000;
            set_state(State::SynSent);
            send_segment(iss, SYN, 0, 0);
            arm_timer();
        }
        if (nonblock)
            return -EINPROGRESS;
        if (long r = sock_wait(this, false, snd_timeout_ns, [&] { return state != State::SynSent; }); r < 0)
            return r == -err::AGAIN ? -EINPROGRESS : r;
        LockGuard guard(g_lock);
        if (state == State::Closed) {
            connected = false;
            long e = error ? error : ECONNREFUSED;
            error = 0;
            return -e;
        }
        return 0;
    }

    long accept(Socket** out, bool nonblock) override {
        for (;;) {
            {
                LockGuard guard(g_lock);
                if (state != State::Listen)
                    return -err::INVAL;
                if (TcpSocket* c = accept_head) {
                    accept_head = c->accept_next;
                    c->accept_next = nullptr;
                    c->in_accept_queue = false;
                    c->parent = nullptr;
                    c->owned = true;
                    queued--;
                    *out = c;
                    return 0;
                }
            }
            if (long r = sock_wait(this, nonblock, rcv_timeout_ns,
                                   [&] { return accept_head != nullptr || state != State::Listen; });
                r < 0)
                return r;
        }
    }

    isize send(const u8* data, usize len, int, const Endpoint*, bool nonblock) override {
        usize done = 0;
        for (;;) {
            {
                LockGuard guard(g_lock);
                if (error && state == State::Closed) {
                    long e = error;
                    return done ? static_cast<isize>(done) : (e == ECONNRESET ? -err::PIPE : -e);
                }
                if (wr_shutdown || fin_queued)
                    return done ? static_cast<isize>(done) : -err::PIPE;
                if (state == State::Closed || state == State::Listen)
                    return done ? static_cast<isize>(done) : (connected ? -err::PIPE : -ENOTCONN);
                if (state == State::Established || state == State::CloseWait) {
                    usize n = min(len - done, sbuf.free());
                    if (n) {
                        sbuf.push(data + done, n);
                        done += n;
                        output();
                    }
                    if (done == len)
                        return static_cast<isize>(done);
                }
            }
            if (long r = sock_wait(this, nonblock, snd_timeout_ns, [&] {
                    return (sbuf.free() > 0 && (state == State::Established || state == State::CloseWait)) ||
                           state == State::Closed || wr_shutdown;
                });
                r < 0)
                return done ? static_cast<isize>(done) : r;
        }
    }

    isize recv(u8* buf, usize len, int flags, Endpoint* from, bool nonblock, usize* full_len) override {
        usize done = 0;
        for (;;) {
            {
                LockGuard guard(g_lock);
                if (from)
                    *from = remote;
                if (state == State::Listen)
                    return -ENOTCONN;
                if (rbuf.len) {
                    usize n = min(len - done, rbuf.len);
                    rbuf.peek(0, buf + done, n);
                    if (!(flags & MSG_PEEK)) {
                        rbuf.drop(n);
                        maybe_window_update();
                    }
                    done += n;
                    if (full_len)
                        *full_len = done;
                    if (done == len || !(flags & MSG_WAITALL) || (flags & MSG_PEEK))
                        return static_cast<isize>(done);
                }
                if (fin_received || rd_shutdown || (state == State::Closed && done))
                    return static_cast<isize>(done);
                if (state == State::Closed) {
                    if (error) {
                        long e = error;
                        error = 0;
                        return -e;
                    }
                    return connected ? 0 : -ENOTCONN;
                }
                if (state == State::SynSent && nonblock)
                    return -err::AGAIN;
            }
            if (long r = sock_wait(this, nonblock, rcv_timeout_ns, [&] {
                    return rbuf.len > 0 || fin_received || rd_shutdown || state == State::Closed;
                });
                r < 0)
                return done ? static_cast<isize>(done) : r;
        }
    }

    // Reading freed a lot of buffer: tell the sender the window opened.
    void maybe_window_update() {
        if (state != State::Established && state != State::FinWait1 && state != State::FinWait2)
            return;
        if (last_window < BUF / 2 && rbuf.free() >= BUF / 2)
            send_ack();
    }

    long shutdown(int how) override {
        LockGuard guard(g_lock);
        if (state == State::Closed || state == State::Listen || state == State::SynSent) {
            if (state == State::Listen)
                close_listener();
            return connected ? 0 : -ENOTCONN;
        }
        if (how == 0 || how == 2) {
            rd_shutdown = true;
            wake();
        }
        if (how == 1 || how == 2)
            start_close();
        return 0;
    }

    // Queue our FIN (half-close).
    void start_close() {
        if (wr_shutdown)
            return;
        wr_shutdown = true;
        if (state == State::Established || state == State::SynReceived) {
            fin_queued = true;
            set_state(State::FinWait1);
        } else if (state == State::CloseWait) {
            fin_queued = true;
            set_state(State::LastAck);
        }
        output();
    }

    void close_listener() {
        for (TcpSocket* s = g_tcp; s; s = s->next) {
            if (s->parent != this)
                continue;
            s->send_reset();
            s->parent = nullptr;
            s->in_accept_queue = false;
            s->accept_next = nullptr;
            s->owned = false;
            s->abort(0);
        }
        accept_head = nullptr;
        queued = 0;
        set_state(State::Closed);
    }

    void close() override {
        LockGuard guard(g_lock);
        owned = false;
        switch (state) {
        case State::Listen:
            close_listener();
            break;
        case State::SynSent:
            set_state(State::Closed);
            break;
        case State::Established:
        case State::SynReceived:
        case State::CloseWait:
            if (rbuf.len) { // unread data: reset, as Linux does
                send_reset();
                abort(0);
            } else {
                start_close();
            }
            break;
        default:
            break;
        }
        reap();
    }

    // Free closed, orphaned sockets (lock held).
    static void reap() {
        TcpSocket** link = &g_tcp;
        while (*link) {
            TcpSocket* s = *link;
            if (!s->owned && !s->in_accept_queue && !s->parent && s->state == State::Closed) {
                *link = s->next;
                delete s;
            } else {
                link = &s->next;
            }
        }
    }

    u16 poll_mask() override {
        using ::poll::POLLERR;
        using ::poll::POLLHUP;
        using ::poll::POLLIN;
        using ::poll::POLLOUT;
        switch (state) {
        case State::Listen: return accept_head ? POLLIN : 0;
        case State::SynSent: case State::SynReceived: return 0;
        case State::Closed:
            return static_cast<u16>(POLLOUT | POLLHUP | (rbuf.len ? POLLIN : 0) | (connected ? POLLIN : 0) |
                                    (error ? POLLERR : 0));
        default: break;
        }
        u16 m = 0;
        if (rbuf.len || fin_received || rd_shutdown)
            m |= POLLIN;
        if ((state == State::Established || state == State::CloseWait) && sbuf.free() && !wr_shutdown)
            m |= POLLOUT;
        if (fin_received && fin_sent)
            m |= POLLHUP;
        if (error)
            m |= POLLERR;
        return m;
    }

    usize bytes_available() override { return rbuf.len; }

    // ---- input ----

    void update_rtt(u32 ack) {
        if (!rtt_timing || seq_lt(ack, rtt_seq + 1))
            return;
        rtt_timing = false;
        u32 sample = static_cast<u32>(clock::now_ms() - rtt_start);
        if (!srtt) {
            srtt = sample;
            rttvar = sample / 2;
        } else {
            u32 diff = srtt > sample ? srtt - sample : sample - srtt;
            rttvar = (3 * rttvar + diff) / 4;
            srtt = (7 * srtt + sample) / 8;
        }
        rto_ms = max<u32>(200, srtt + max<u32>(10, 4 * rttvar));
        if (rto_ms > 60000)
            rto_ms = 60000;
    }

    void process_ack(u32 ack, u16 wnd) {
        if (seq_gt(ack, snd_una) && seq_le(ack, snd_nxt)) {
            u32 acked = ack - snd_una;
            u32 data_acked = acked;
            if (fin_sent && seq_gt(ack, fin_seq))
                data_acked--;
            data_acked = min<u32>(data_acked, static_cast<u32>(sbuf.len));
            sbuf.drop(data_acked);
            sbuf_seq += data_acked;
            update_rtt(ack);
            snd_una = ack;
            retries = 0;
            cwnd = min<u32>(cwnd + mss, 1024 * 1024);
            rtx_deadline = snd_una == snd_nxt ? 0 : clock::now_ms() + rto_ms;
            wake();
        }
        if (seq_le(snd_una, ack))
            snd_wnd = wnd;
    }

    void input(const u8* seg, usize len, u32 src, u32 dst) {
        usize hl = (seg[12] >> 4) * 4u;
        u8 flags = seg[13];
        u32 seq = get32(seg + 4), ack = get32(seg + 8);
        u16 wnd = get16(seg + 14);
        const u8* data = seg + hl;
        usize dlen = len - hl;

        if (state == State::Listen) {
            if (flags & RST)
                return;
            if (flags & ACK) {
                reset_reply(src, dst, seg, len);
                return;
            }
            if (!(flags & SYN) || queued >= backlog)
                return;
            auto* c = new TcpSocket;
            if (!c->ok()) {
                c->owned = false;
                c->state = State::Closed;
                reap();
                return;
            }
            c->owned = false;
            c->parent = this;
            c->bound = true;
            c->connected = true;
            c->local = Endpoint{dst, local.port};
            c->remote = Endpoint{src, get16(seg)};
            c->irs = seq;
            c->rcv_nxt = seq + 1;
            c->iss = random_iss();
            c->snd_una = c->iss;
            c->snd_nxt = c->iss + 1;
            c->sbuf_seq = c->iss + 1;
            c->snd_wnd = wnd;
            c->mss = parse_mss(seg, hl);
            c->cwnd = 10 * c->mss;
            c->set_state(State::SynReceived);
            c->send_segment(c->iss, SYN | ACK, 0, 0);
            c->arm_timer();
            return;
        }

        if (state == State::SynSent) {
            if ((flags & ACK) && ack != snd_nxt) {
                if (!(flags & RST))
                    reset_reply(src, dst, seg, len);
                return;
            }
            if (flags & RST) {
                if (flags & ACK)
                    abort(static_cast<int>(ECONNREFUSED));
                return;
            }
            if ((flags & SYN) && (flags & ACK)) {
                irs = seq;
                rcv_nxt = seq + 1;
                mss = parse_mss(seg, hl);
                cwnd = 10 * mss;
                snd_una = ack;
                snd_wnd = wnd;
                rtx_deadline = 0;
                retries = 0;
                set_state(State::Established);
                send_ack();
                output();
            }
            return;
        }

        // Synchronized states: is the segment acceptable?
        u32 rwnd = static_cast<u32>(rbuf.free());
        bool acceptable = dlen == 0 ? (seq == rcv_nxt || (rwnd && seq_le(rcv_nxt, seq) && seq_lt(seq, rcv_nxt + rwnd)))
                                    : (seq_lt(seq + static_cast<u32>(dlen) - 1, rcv_nxt + max<u32>(rwnd, 1)) &&
                                       seq_le(rcv_nxt, seq + static_cast<u32>(dlen) - 1 + 1));
        if (flags & RST) {
            if (seq_le(rcv_nxt, seq) && seq_lt(seq, rcv_nxt + max<u32>(rwnd, 1))) {
                if (state == State::SynReceived && parent) {
                    state = State::Closed; // never surfaced to the application
                    parent = nullptr;
                    reap();
                    return;
                }
                abort(state == State::CloseWait || state == State::Established || state == State::FinWait1 ||
                              state == State::FinWait2
                          ? static_cast<int>(ECONNRESET)
                          : 0);
            }
            return;
        }
        if (!acceptable && !(dlen == 0 && seq_lt(seq, rcv_nxt))) {
            send_ack(); // out of window / out of order: re-announce where we are
            return;
        }
        if (flags & SYN) { // SYN in a synchronized state: challenge ACK
            send_ack();
            return;
        }
        if (!(flags & ACK))
            return;

        if (state == State::SynReceived) {
            if (!seq_le(snd_una, ack) || !seq_le(ack, snd_nxt)) {
                reset_reply(src, dst, seg, len);
                return;
            }
            snd_una = ack;
            snd_wnd = wnd;
            rtx_deadline = 0;
            retries = 0;
            set_state(State::Established);
            if (TcpSocket* l = parent) { // hand it to accept()
                in_accept_queue = true;
                TcpSocket** tail = &l->accept_head;
                while (*tail)
                    tail = &(*tail)->accept_next;
                *tail = this;
                l->queued++;
                l->wake();
            }
        }

        process_ack(ack, wnd);

        // Our FIN acknowledged?
        bool fin_acked = fin_sent && seq_gt(snd_una, fin_seq);
        if (fin_acked) {
            if (state == State::FinWait1)
                set_state(State::FinWait2);
            else if (state == State::Closing)
                enter_time_wait();
            else if (state == State::LastAck) {
                set_state(State::Closed);
                reap();
                return;
            }
        }

        bool need_ack = false;
        if (dlen && (state == State::Established || state == State::FinWait1 || state == State::FinWait2)) {
            u32 skip = seq_lt(seq, rcv_nxt) ? rcv_nxt - seq : 0;
            if (skip < dlen) {
                usize n = min<usize>(dlen - skip, rbuf.free());
                if (!rd_shutdown)
                    rbuf.push(data + skip, n);
                rcv_nxt += static_cast<u32>(n);
                if (n)
                    wake();
                if (n < dlen - skip)
                    flags &= static_cast<u8>(~FIN); // didn't take everything: FIN isn't in order yet
            }
            need_ack = true;
        }

        if ((flags & FIN) && seq + static_cast<u32>(dlen) == rcv_nxt) {
            rcv_nxt++;
            fin_received = true;
            need_ack = true;
            switch (state) {
            case State::Established: set_state(State::CloseWait); break;
            case State::FinWait1:
                if (fin_acked)
                    enter_time_wait();
                else
                    set_state(State::Closing);
                break;
            case State::FinWait2: enter_time_wait(); break;
            default: break;
            }
            wake();
        }
        if (need_ack)
            send_ack();
        output();
    }

    void enter_time_wait() {
        rtx_deadline = 0;
        set_state(State::TimeWait);
    }

    static u32 parse_mss(const u8* seg, usize hl) {
        for (usize i = 20; i + 1 < hl;) {
            u8 kind = seg[i];
            if (kind == 0)
                break;
            if (kind == 1) {
                i++;
                continue;
            }
            u8 olen = seg[i + 1];
            if (olen < 2 || i + olen > hl)
                break;
            if (kind == 2 && olen == 4)
                return min<u32>(get16(seg + i + 2), OUR_MSS);
            i += olen;
        }
        return 536;
    }

    // RST for a segment that has no (valid) connection.
    static void reset_reply(u32 src, u32 dst, const u8* seg, usize len) {
        u8 flags = seg[13];
        if (flags & RST)
            return;
        usize hl = (seg[12] >> 4) * 4u;
        u8 r[20] = {};
        put16(r, get16(seg + 2));
        put16(r + 2, get16(seg));
        if (flags & ACK) {
            put32(r + 4, get32(seg + 8));
            r[13] = RST;
        } else {
            u32 seg_len = static_cast<u32>(len - hl) + ((flags & SYN) ? 1 : 0) + ((flags & FIN) ? 1 : 0);
            put32(r + 8, get32(seg + 4) + seg_len);
            r[13] = RST | ACK;
        }
        r[12] = 5 << 4;
        put16(r + 16, pseudo_checksum(dst, src, PROTO_TCP, r, 20));
        ip_send(dst, src, PROTO_TCP, r, 20);
    }
};

u16 g_next_port = 49152;

} // namespace

Socket* create_tcp() {
    LockGuard guard(g_lock);
    auto* s = new TcpSocket;
    if (!s->ok()) {
        s->owned = false;
        TcpSocket::reap();
        return nullptr;
    }
    return s;
}

bool port_in_use(u8 proto, u32 addr, u16 port) {
    if (proto == PROTO_UDP)
        return port_in_use_udp(addr, port);
    for (TcpSocket* s = g_tcp; s; s = s->next)
        if (s->bound && s->local.port == port && s->state != State::Closed &&
            (!s->local.addr || !addr || s->local.addr == addr))
            return true;
    return false;
}

u16 ephemeral_port(u8 proto) {
    for (u32 i = 0; i < 16384; i++) {
        u16 p = g_next_port;
        g_next_port = g_next_port == 65535 ? 49152 : static_cast<u16>(g_next_port + 1);
        if (!port_in_use(proto, 0, p))
            return p;
    }
    return 0;
}

void tcp_input(Interface* in, const u8* ip, const u8* seg, usize len) {
    if (len < 20)
        return;
    usize hl = (seg[12] >> 4) * 4u;
    if (hl < 20 || hl > len)
        return;
    u32 src = get32(ip + 12), dst = get32(ip + 16);
    if (!(in->flags & IFF_LOOPBACK) && pseudo_checksum(src, dst, PROTO_TCP, seg, len) != 0)
        return;
    u16 sport = get16(seg), dport = get16(seg + 2);
    TcpSocket* match = nullptr;
    TcpSocket* listener = nullptr;
    for (TcpSocket* s = g_tcp; s; s = s->next) {
        if (s->local.port != dport || s->state == State::Closed)
            continue;
        if (s->state == State::Listen) {
            if (!s->local.addr || s->local.addr == dst)
                listener = s;
            continue;
        }
        if (s->remote.port == sport && s->remote.addr == src && (!s->local.addr || s->local.addr == dst)) {
            match = s;
            break;
        }
    }
    TcpSocket* s = match ? match : listener;
    if (!s) {
        TcpSocket::reset_reply(src, dst, seg, len);
        return;
    }
    s->input(seg, len, src, dst);
}

void tcp_timer(u64 now) {
    for (TcpSocket* s = g_tcp; s; s = s->next) {
        if (s->rtx_deadline && now >= s->rtx_deadline)
            s->on_timeout();
        if (s->state == State::TimeWait && now - s->state_since >= TIME_WAIT_MS)
            s->set_state(State::Closed);
        if (s->state == State::FinWait2 && !s->owned && now - s->state_since >= FIN_WAIT2_ORPHAN_MS)
            s->set_state(State::Closed);
    }
    TcpSocket::reap();
}

void for_each_tcp(void (*fn)(Socket*, int, void*), void* ctx) {
    for (TcpSocket* s = g_tcp; s; s = s->next)
        fn(s, linux_state(s->state), ctx);
}

} // namespace net
