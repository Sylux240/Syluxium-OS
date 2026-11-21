// TCP (RFC 9293 essentials) over IPv4 and IPv6: connection setup and
// teardown, retransmission with an RTT-based timeout (RFC 6298), flow
// control with window scaling (RFC 7323), congestion control (slow start,
// congestion avoidance, fast retransmit / NewReno fast recovery, RFC 5681
// and 6582), out-of-order segment queueing, delayed ACKs, listen/accept and
// half-close.
//
// Not yet: SACK, timestamps, keep-alives, path MTU discovery.

#include <core/log.hpp>
#include <mm/heap.hpp>
#include <net/internal.hpp>
#include <time/clock.hpp>

namespace net {

bool port_in_use_udp(const Endpoint& addr, u16 port, bool v6only);

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
// Big buffers keep a fast sender busy: the receive window (scaled) can
// cover a whole buffer, so throughput isn't capped at 64 KiB per RTT.
constexpr usize RCV_BUF = 512 * 1024;
constexpr usize SND_BUF = 256 * 1024;
constexpr u8 RCV_WSCALE = 4;          // 65535 << 4 > RCV_BUF
constexpr u32 MAX_MSS = 1460;         // largest segment we build (IPv4 over 1500-byte MTU)
constexpr usize MAX_HDR = 60;
constexpr u64 TIME_WAIT_MS = 4000;
constexpr u64 FIN_WAIT2_ORPHAN_MS = 60000;
constexpr u64 DELAYED_ACK_MS = 20;

bool seq_lt(u32 a, u32 b) { return static_cast<i32>(a - b) < 0; }
bool seq_le(u32 a, u32 b) { return static_cast<i32>(a - b) <= 0; }
bool seq_gt(u32 a, u32 b) { return static_cast<i32>(a - b) > 0; }
bool seq_ge(u32 a, u32 b) { return static_cast<i32>(a - b) >= 0; }

u32 random_iss() {
    static u32 counter = 0x5EED;
    u64 t = clock::now_ns();
    counter = counter * 1103515245u + 12345u;
    return static_cast<u32>(t ^ (t >> 29)) ^ counter;
}

// Byte ring buffer (copies in at most two pieces).
struct Ring {
    u8* data = nullptr;
    usize cap = 0, head = 0, len = 0;
    usize free() const { return cap - len; }
    void push(const u8* src, usize n) {
        if (!n)
            return; // also covers released buffers (cap 0)
        usize tail = (head + len) % cap;
        usize first = min(n, cap - tail);
        memcpy(data + tail, src, first);
        memcpy(data, src + first, n - first);
        len += n;
    }
    void peek(usize offset, u8* dst, usize n) const {
        if (!n)
            return;
        usize start = (head + offset) % cap;
        usize first = min(n, cap - start);
        memcpy(dst, data + start, first);
        memcpy(dst + first, data, n - first);
    }
    void drop(usize n) {
        if (!n)
            return;
        head = (head + n) % cap;
        len -= n;
    }
};

// A segment that arrived ahead of a gap, waiting for the gap to fill.
struct OooSegment {
    OooSegment* next;
    u32 seq, len;
    bool fin;
    u8 data[];
};

class TcpSocket;
TcpSocket* g_tcp = nullptr;

class TcpSocket : public Socket {
public:
    explicit TcpSocket(u16 d) : Socket(SOCK_STREAM, PROTO_TCP, d) {
        sbuf.data = static_cast<u8*>(kmalloc(SND_BUF));
        sbuf.cap = SND_BUF;
        rbuf.data = static_cast<u8*>(kmalloc(RCV_BUF));
        rbuf.cap = RCV_BUF;
        next = g_tcp;
        g_tcp = this; // caller holds g_lock
    }
    ~TcpSocket() override {
        kfree(sbuf.data);
        kfree(rbuf.data);
        drop_ooo();
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
    u32 snd_wnd = 0, mss = 536;
    u8 snd_wscale = 0;               // shift for windows the peer sends us
    bool wscale_ok = false;          // both sides sent the window scale option
    bool fin_queued = false, fin_sent = false;
    u32 fin_seq = 0;
    bool wr_shutdown = false;
    // Congestion control.
    u32 cwnd = 0, ssthresh = 0x7FFFFFFF;
    int dupacks = 0;
    bool in_recovery = false;
    u32 recover = 0;                 // snd_nxt when fast recovery started
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
    u32 last_window = 0;             // bytes, as last advertised
    OooSegment* ooo = nullptr;
    usize ooo_bytes = 0;
    int unacked_segments = 0;        // delayed ACK bookkeeping
    u64 ack_deadline = 0;
    u64 state_since = 0;

    bool ok() const { return sbuf.data && rbuf.data; }

    u8 rcv_wscale() const { return wscale_ok ? RCV_WSCALE : 0; }

    // The window field to advertise (unscaled in SYN segments).
    u16 window_field(bool syn) const {
        usize w = rbuf.free();
        if (!syn)
            w >>= rcv_wscale();
        return static_cast<u16>(min<usize>(w, 65535));
    }

    void set_state(State s) {
        state = s;
        state_since = clock::now_ms();
        wake();
        if (parent)
            parent->wake();
    }

    // Build and send one segment. `data_offset` indexes sbuf.
    void send_segment(u32 seq, u8 flags, usize data_offset, usize len) {
        u8 seg[MAX_HDR + MAX_MSS];
        usize hl = 20;
        if (flags & SYN) { // MSS, and window scaling when offering it / the peer offered it
            seg[20] = 2;
            seg[21] = 4;
            put16(seg + 22, static_cast<u16>(path_mss(remote)));
            hl = 24;
            if (state == State::SynSent || wscale_ok) {
                seg[24] = 1; // NOP
                seg[25] = 3;
                seg[26] = 3;
                seg[27] = RCV_WSCALE;
                hl = 28;
            }
        }
        put16(seg, local.port);
        put16(seg + 2, remote.port);
        put32(seg + 4, seq);
        put32(seg + 8, (flags & ACK) ? rcv_nxt : 0);
        seg[12] = static_cast<u8>((hl / 4) << 4);
        seg[13] = flags;
        put16(seg + 14, window_field(flags & SYN));
        put16(seg + 16, 0);
        put16(seg + 18, 0);
        if (len)
            sbuf.peek(data_offset, seg + hl, len);
        put16(seg + 16, l4_checksum(local, remote, PROTO_TCP, seg, hl + len));
        ip_output(local, remote, PROTO_TCP, seg, hl + len);
        if (flags & ACK) {
            last_window = static_cast<u32>(rbuf.free());
            unacked_segments = 0; // every ACK-carrying segment acknowledges
            ack_deadline = 0;
        }
    }

    void send_ack() { send_segment(snd_nxt, ACK, 0, 0); }

    void arm_timer() {
        if (!rtx_deadline)
            rtx_deadline = clock::now_ms() + rto_ms;
    }

    bool sending_state() const {
        return state == State::Established || state == State::CloseWait || state == State::FinWait1 ||
               state == State::Closing || state == State::LastAck;
    }

    // Send whatever the windows allow.
    void output(bool probe = false) {
        if (!sending_state())
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
                // Avoid silly small segments while plenty is in flight (RFC 9293 3.8.6.2.1).
                if (n < mss && n < sbuf.len - offset && inflight > 0 && !probe)
                    break;
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

    // Resend the oldest unacknowledged segment (fast retransmit).
    void retransmit_first() {
        usize n = min<usize>(min<usize>(sbuf.len, mss), snd_nxt - snd_una);
        if (n && snd_una == sbuf_seq)
            send_segment(snd_una, ACK, 0, n);
        else if (fin_sent && snd_una == fin_seq)
            send_segment(fin_seq, FIN | ACK, 0, 0);
        rtt_timing = false;
    }

    u32 flight_size() const { return snd_nxt - snd_una; }

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
        // Go back to the oldest unacknowledged byte and restart slow start.
        bool window_probe = snd_wnd == 0;
        if (!window_probe)
            ssthresh = max<u32>(flight_size() / 2, 2 * mss);
        snd_nxt = snd_una;
        if (fin_sent && seq_le(snd_una, fin_seq))
            fin_sent = false;
        cwnd = mss;
        in_recovery = false;
        dupacks = 0;
        output(window_probe);
    }

    // Drop the connection (reset received, timeout, abort on close).
    void abort(int err_code) {
        error = err_code;
        rtx_deadline = 0;
        ack_deadline = 0;
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
        if (!ep.any() && !is_local_endpoint(ep))
            return -EADDRNOTAVAIL;
        u16 port = ep.port;
        if (!port) {
            port = ephemeral_port(PROTO_TCP);
            if (!port)
                return -EADDRINUSE;
        } else if (conflicts(ep, port)) {
            return -EADDRINUSE;
        }
        local = ep;
        local.port = port;
        if (domain == AF_INET6 && ep.any() && !ep.v6)
            local.v6 = true; // implicit bind of an AF_INET6 socket: the dual-stack wildcard ::
        bound = true;
        return 0;
    }

    bool conflicts(const Endpoint& addr, u16 port) {
        for (TcpSocket* s = g_tcp; s; s = s->next) {
            if (s == this || !s->bound || s->local.port != port || s->state == State::Closed)
                continue;
            if (!bindings_overlap(s->local, addr, s->v6only, v6only))
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

    // Fresh send-side state for a new connection.
    void init_send(u32 peer_wnd) {
        iss = random_iss();
        snd_una = iss;
        snd_nxt = iss + 1;
        sbuf_seq = iss + 1;
        snd_wnd = peer_wnd;
        cwnd = 10 * 536;
        ssthresh = 0x7FFFFFFF;
        retries = 0;
        rto_ms = 1000;
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
            if (error) // a previous attempt failed: report it once, then allow a retry
                error = 0;
            if (ep.any() || !ep.port)
                return -ECONNREFUSED;
            Endpoint src;
            if (!source_endpoint(ep, &src))
                return -ENETUNREACH;
            if (!bound)
                if (long r = bind_locked(Endpoint{}); r < 0)
                    return r;
            if (local.any()) {
                u16 port = local.port;
                local = src;
                local.port = port;
            } else if (local.v6 != ep.v6) {
                return -err::INVAL;
            }
            remote = ep;
            connected = true;
            wscale_ok = false;
            init_send(0);
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

    // Reading freed buffer space: tell the sender once the window grew by
    // two segments or half the buffer (receiver-side SWS avoidance).
    void maybe_window_update() {
        if (state != State::Established && state != State::FinWait1 && state != State::FinWait2)
            return;
        usize now_free = rbuf.free();
        if (now_free >= last_window + max<usize>(2 * mss, RCV_BUF / 4) || (last_window < mss && now_free >= mss))
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

    void process_ack(u32 ack, u32 wnd, bool pure_ack) {
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
            dupacks = 0;
            if (in_recovery) {
                if (seq_ge(ack, recover)) { // full ACK: leave fast recovery
                    in_recovery = false;
                    cwnd = ssthresh;
                } else { // partial ACK (NewReno): the next hole is lost too
                    retransmit_first();
                    cwnd = cwnd > acked ? cwnd - acked + mss : mss;
                }
            } else if (cwnd < ssthresh) {
                cwnd += min<u32>(acked, mss); // slow start
            } else {
                cwnd += max<u32>(mss * mss / cwnd, 1); // congestion avoidance
            }
            cwnd = min<u32>(cwnd, static_cast<u32>(SND_BUF));
            rtx_deadline = snd_una == snd_nxt ? 0 : clock::now_ms() + rto_ms;
            wake();
        } else if (ack == snd_una && pure_ack && wnd == snd_wnd && snd_nxt != snd_una) {
            // Duplicate ACK: the peer got something beyond a hole.
            if (++dupacks == 3 && !in_recovery) {
                ssthresh = max<u32>(flight_size() / 2, 2 * mss);
                in_recovery = true;
                recover = snd_nxt;
                retransmit_first();
                cwnd = ssthresh + 3 * mss;
            } else if (dupacks > 3 && in_recovery) {
                cwnd += mss; // each dup ACK means a segment left the network
            }
        }
        if (seq_le(snd_una, ack))
            snd_wnd = wnd;
    }

    void drop_ooo() {
        while (ooo) {
            OooSegment* s = ooo;
            ooo = s->next;
            kfree(s);
        }
        ooo_bytes = 0;
    }

    // Keep a segment that arrived beyond rcv_nxt (sorted by sequence).
    void queue_ooo(u32 seq, const u8* data, usize len, bool fin) {
        if (seq_gt(seq + static_cast<u32>(len), rcv_nxt + static_cast<u32>(rbuf.free())) ||
            ooo_bytes + len > rbuf.free())
            return; // beyond what we can take: the peer will resend it
        OooSegment** link = &ooo;
        while (*link && seq_lt((*link)->seq, seq))
            link = &(*link)->next;
        if (*link && (*link)->seq == seq && (*link)->len >= len)
            return; // duplicate
        auto* s = static_cast<OooSegment*>(kmalloc(sizeof(OooSegment) + len));
        if (!s)
            return;
        s->seq = seq;
        s->len = static_cast<u32>(len);
        s->fin = fin;
        memcpy(s->data, data, len);
        s->next = *link;
        *link = s;
        ooo_bytes += len;
    }

    // Move queued segments that are now in order into rbuf. Returns true
    // if a queued FIN became in order.
    bool drain_ooo() {
        bool fin = false;
        while (ooo && seq_le(ooo->seq, rcv_nxt)) {
            OooSegment* s = ooo;
            ooo = s->next;
            ooo_bytes -= s->len;
            u32 skip = rcv_nxt - s->seq;
            if (skip < s->len) {
                usize n = min<usize>(s->len - skip, rbuf.free());
                if (!rd_shutdown)
                    rbuf.push(s->data + skip, n);
                rcv_nxt += static_cast<u32>(n);
                if (s->fin && n == s->len - skip)
                    fin = true;
            } else if (s->fin && skip == s->len) {
                fin = true;
            }
            kfree(s);
            if (fin)
                break;
        }
        return fin;
    }

    static void parse_options(const u8* seg, usize hl, u32* mss_out, int* wscale_out) {
        *mss_out = 536;
        *wscale_out = -1;
        for (usize i = 20; i < hl;) {
            u8 kind = seg[i];
            if (kind == 0)
                break;
            if (kind == 1) {
                i++;
                continue;
            }
            if (i + 1 >= hl)
                break;
            u8 olen = seg[i + 1];
            if (olen < 2 || i + olen > hl)
                break;
            if (kind == 2 && olen == 4)
                *mss_out = get16(seg + i + 2);
            else if (kind == 3 && olen == 3)
                *wscale_out = min<u8>(seg[i + 2], 14);
            i += olen;
        }
    }

    // Apply the peer's SYN options (lock held).
    void take_syn_options(const u8* seg, usize hl) {
        u32 peer_mss;
        int ws;
        parse_options(seg, hl, &peer_mss, &ws);
        mss = min<u32>(min<u32>(peer_mss, path_mss(remote)), MAX_MSS);
        if (mss < 64)
            mss = 64;
        wscale_ok = ws >= 0; // active open: we offered it; passive: we answer in kind
        snd_wscale = wscale_ok ? static_cast<u8>(ws) : 0;
    }

    void input(const u8* seg, usize len, const Endpoint& src, const Endpoint& dst) {
        usize hl = (seg[12] >> 4) * 4u;
        u8 flags = seg[13];
        u32 seq = get32(seg + 4), ack = get32(seg + 8);
        u32 raw_wnd = get16(seg + 14);
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
            auto* c = new TcpSocket(domain);
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
            c->local = dst;
            c->local.port = local.port;
            c->remote = src;
            c->remote.port = get16(seg);
            c->irs = seq;
            c->rcv_nxt = seq + 1;
            c->init_send(raw_wnd); // SYN windows are never scaled
            c->take_syn_options(seg, hl);
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
                take_syn_options(seg, hl);
                cwnd = 10 * mss;
                snd_una = ack;
                snd_wnd = raw_wnd;
                rtx_deadline = 0;
                retries = 0;
                set_state(State::Established);
                send_ack();
                output();
            }
            return;
        }

        u32 wnd = raw_wnd << snd_wscale;

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
            send_ack(); // out of window: re-announce where we are
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

        process_ack(ack, wnd, dlen == 0 && !(flags & FIN));

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

        bool ack_now = false, need_ack = false;
        bool fin_in_order = false;
        bool receiving = state == State::Established || state == State::FinWait1 || state == State::FinWait2;
        if (dlen && receiving) {
            if (seq_gt(seq, rcv_nxt)) { // a hole before this segment: keep it, and say so at once
                queue_ooo(seq, data, dlen, flags & FIN);
                ack_now = true;
                flags &= static_cast<u8>(~FIN);
            } else {
                u32 skip = rcv_nxt - seq;
                if (skip < dlen) {
                    usize n = min<usize>(dlen - skip, rbuf.free());
                    if (!rd_shutdown)
                        rbuf.push(data + skip, n);
                    rcv_nxt += static_cast<u32>(n);
                    if (n < dlen - skip)
                        flags &= static_cast<u8>(~FIN); // didn't take everything: FIN isn't in order yet
                    if (ooo) {
                        fin_in_order = drain_ooo();
                        ack_now = true; // a hole was filled: tell the sender right away
                    }
                    if (n)
                        wake();
                } else {
                    ack_now = true; // a pure retransmission
                }
                need_ack = true;
            }
        }

        if (((flags & FIN) && seq + static_cast<u32>(dlen) == rcv_nxt) || fin_in_order) {
            rcv_nxt++;
            fin_received = true;
            ack_now = true;
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
        output(); // data we send carries the ACK
        if (ack_now) {
            send_ack();
        } else if (need_ack) {
            // Delayed ACK (RFC 9293 3.8.6.3): every second segment, or after DELAYED_ACK_MS.
            if (++unacked_segments >= 2)
                send_ack();
            else if (!ack_deadline)
                ack_deadline = clock::now_ms() + DELAYED_ACK_MS;
        }
    }

    void enter_time_wait() {
        rtx_deadline = 0;
        drop_ooo();
        set_state(State::TimeWait);
    }

    // RST for a segment that has no (valid) connection.
    static void reset_reply(const Endpoint& src, const Endpoint& dst, const u8* seg, usize len) {
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
        put16(r + 16, l4_checksum(dst, src, PROTO_TCP, r, 20));
        ip_output(dst, src, PROTO_TCP, r, 20);
    }
};

u16 g_next_port = 49152;

} // namespace

Socket* create_tcp(u16 domain) {
    LockGuard guard(g_lock);
    auto* s = new TcpSocket(domain);
    if (!s->ok()) {
        s->owned = false;
        TcpSocket::reap();
        return nullptr;
    }
    return s;
}

bool port_in_use(u8 proto, const Endpoint& addr, u16 port, bool v6only) {
    if (proto == PROTO_UDP)
        return port_in_use_udp(addr, port, v6only);
    for (TcpSocket* s = g_tcp; s; s = s->next)
        if (s->bound && s->local.port == port && s->state != State::Closed &&
            bindings_overlap(s->local, addr, s->v6only, v6only))
            return true;
    return false;
}

u16 ephemeral_port(u8 proto) {
    for (u32 i = 0; i < 16384; i++) {
        u16 p = g_next_port;
        g_next_port = g_next_port == 65535 ? 49152 : static_cast<u16>(g_next_port + 1);
        if (!port_in_use(proto, Endpoint{}, p))
            return p;
    }
    return 0;
}

void tcp_input(Interface* in, const Endpoint& src, const Endpoint& dst, const u8* seg, usize len) {
    if (len < 20)
        return;
    usize hl = (seg[12] >> 4) * 4u;
    if (hl < 20 || hl > len)
        return;
    if (!(in->flags & IFF_LOOPBACK) && l4_checksum(src, dst, PROTO_TCP, seg, len) != 0)
        return;
    u16 sport = get16(seg), dport = get16(seg + 2);
    TcpSocket* match = nullptr;
    TcpSocket* listener = nullptr;
    for (TcpSocket* s = g_tcp; s; s = s->next) {
        if (s->local.port != dport || s->state == State::Closed)
            continue;
        if (s->state == State::Listen) {
            if (local_accepts(s, dst) && (!listener || !s->local.any()))
                listener = s; // a specific address beats a wildcard
            continue;
        }
        if (s->remote.port == sport && s->remote.same_addr(src) && (s->local.any() || s->local.same_addr(dst))) {
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
        if (s->ack_deadline && now >= s->ack_deadline) {
            s->ack_deadline = 0;
            if (s->state != State::Closed && s->state != State::Listen && s->state != State::SynSent)
                s->send_ack();
        }
        if (s->state == State::TimeWait && !s->owned && s->rbuf.data) {
            // Nobody will read or write again: TIME_WAIT only needs the
            // sequence numbers, so give the big buffers back now.
            kfree(s->sbuf.data);
            kfree(s->rbuf.data);
            s->sbuf = Ring{};
            s->rbuf = Ring{};
        }
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
