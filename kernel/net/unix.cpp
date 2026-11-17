// AF_UNIX (local) sockets: SOCK_STREAM, SOCK_DGRAM and SOCK_SEQPACKET,
// filesystem and abstract names, socketpair, listen/accept, passing file
// descriptors (SCM_RIGHTS) and credentials (SCM_CREDENTIALS, SO_PEERCRED).
// D-Bus, X11 and Wayland all talk over these.
//
// Design: every socket owns a receive queue of messages. A stream socket
// queues each write as one chunk (so passed files stay attached to the
// bytes they were sent with); datagram and seqpacket sockets queue whole
// messages. Writers append straight into the receiver's queue, so a
// connection is just two sockets pointing at each other.
//
// One mutex (g_unix) protects all Unix socket state. Blocking uses a
// per-socket event counter: whoever changes something a sleeper may care
// about bumps the counter of the affected socket and wakes it (kick()).
// Sleepers re-check everything under the mutex afterwards, so they never
// touch another socket that may have been freed meanwhile.

#include <core/log.hpp>
#include <fs/genfs.hpp>
#include <fs/ramfs.hpp>
#include <lib/format.hpp>
#include <mm/heap.hpp>
#include <net/internal.hpp>
#include <proc/process.hpp>

namespace net {

namespace {

constexpr usize QUEUE_LIMIT = 256 * KiB; // bytes queued at one receiver
constexpr usize DGRAM_QUEUE_MAX = 512;   // messages queued at one datagram receiver
constexpr usize DGRAM_MAX = 212992;      // Linux's default wmem_max
constexpr usize PATH_MAX_LEN = 108;      // sizeof(sun_path)
constexpr u16 POLLRDHUP = 0x2000;

sched::Mutex g_unix;

// One queued message (datagram / seqpacket record) or stream chunk.
struct UnixMsg {
    UnixMsg* next;
    usize len, off; // off: bytes already read (stream)
    SockAddr from;  // sender's name (datagrams); len 0 if it had none
    Control ctl;    // passed files (owned)
    Ucred cred;     // sender's (or the claimed SCM_CREDENTIALS) credentials
    u8 data[];
};

void free_msg(UnixMsg* m) {
    release_control(m->ctl);
    kfree(m);
}

// Messages freed only once g_unix is released: dropping the last reference
// of a passed file may close another Unix socket, which needs g_unix.
// Declare it before the LockGuard so it is destroyed after the unlock.
struct Graveyard {
    UnixMsg* head = nullptr;
    void add(UnixMsg* m) {
        m->next = head;
        head = m;
    }
    ~Graveyard() {
        while (head) {
            UnixMsg* m = head;
            head = m->next;
            free_msg(m);
        }
    }
};

// The filesystem entry bind() leaves behind (ls shows it as 's').
class SockNode : public vfs::Inode {
public:
    explicit SockNode(u32 perms) {
        ino = vfs::next_ino();
        mode = vfs::S_IFSOCK | (perms & 07777);
        atime = mtime = ctime = vfs::now();
    }
    isize read(u64, void*, usize) override { return -err::NXIO; }
    isize write(u64, const void*, usize) override { return -err::NXIO; }
};

vfs::Inode* cwd() {
    proc::Process* p = proc::current();
    return p && p->cwd ? p->cwd : vfs::root();
}

// The NUL-terminated path of a filesystem sockaddr_un.
void extract_path(const SockAddr& sa, char* out) {
    usize n = 0;
    while (n < PATH_MAX_LEN && 2 + n < sa.len && sa.data[2 + n])
        n++;
    memcpy(out, sa.data + 2, n);
    out[n] = 0;
}

enum class St : u8 { Unconnected, Listening, Connected, Disconnected };

class UnixSocket;
UnixSocket* g_all = nullptr; // every Unix socket, for name lookups (g_unix held)
u32 g_autobind = 0;

void kick(Socket* s);

class UnixSocket : public Socket {
public:
    explicit UnixSocket(int t) : Socket(t, 0, AF_UNIX) {
        next_all = g_all; // caller holds g_unix
        g_all = this;
    }

    UnixSocket* next_all = nullptr;
    St st = St::Unconnected;
    UnixSocket* peer = nullptr;        // stream/seqpacket: other end; datagram: default destination
    bool peer_dead = false;            // datagram peer went away (send gives ECONNREFUSED)
    bool owned = true;                 // a file refers to it (false: waiting in an accept queue)
    bool has_name = false;             // getsockname() result is `name`
    SockAddr name;
    u64 node_dev = 0, node_ino = 0;    // filesystem node of a path name (0 = abstract / none)
    bool rcv_shut = false, snd_shut = false;
    bool passcred = false;             // SO_PASSCRED
    bool waiting_space = false;        // blocked until some receiver/listener frees room
    Ucred peer_cred;                   // SO_PEERCRED
    bool has_peer_cred = false;
    Ucred listen_cred;                 // our credentials at listen() time, for connecting clients
    u64 events = 0;                    // bumped by kick()

    // Receive queue.
    UnixMsg* head = nullptr;
    UnixMsg* tail = nullptr;
    usize qbytes = 0, qcount = 0;

    // Listener: connected but not yet accepted server-side sockets.
    UnixSocket* acc_head = nullptr;
    UnixSocket* acc_next = nullptr;
    int backlog = 0, nqueued = 0;

    bool connection() const { return type != SOCK_DGRAM; }

    void enqueue(UnixMsg* m) {
        m->next = nullptr;
        if (tail)
            tail->next = m;
        else
            head = m;
        tail = m;
        qbytes += m->len - m->off;
        qcount++;
        kick(this);
    }

    UnixMsg* dequeue() {
        UnixMsg* m = head;
        head = m->next;
        if (!head)
            tail = nullptr;
        qbytes -= m->len - m->off;
        qcount--;
        return m;
    }

    // Someone may be blocked until this queue drains.
    void room_freed() {
        if (peer)
            kick(peer);
        if (!connection())
            wake_space_waiters();
    }

    static void wake_space_waiters() {
        for (UnixSocket* s = g_all; s; s = s->next_all)
            if (s->waiting_space) {
                s->waiting_space = false;
                kick(s);
            }
    }

    u64 snapshot() const { return events; }

    // Sleep (g_unix NOT held) until kick() was called on us after `seen`.
    long wait(u64 seen, bool nonblock, u64 timeout_ns) {
        return sock_wait(this, nonblock, timeout_ns,
                         [&] { return __atomic_load_n(&events, __ATOMIC_ACQUIRE) != seen; });
    }

    // ---- naming ----

    long bind_addr(const SockAddr& sa) override {
        if (sa.family() != AF_UNIX)
            return -err::INVAL;
        LockGuard guard(g_unix);
        if (has_name)
            return -err::INVAL;
        if (sa.len <= 2)
            return autobind();
        if (sa.data[2] == 0) { // abstract namespace: the name is every byte after the family
            if (find_abstract(sa))
                return -EADDRINUSE;
            name = sa;
        } else {
            if (long r = bind_path(sa); r < 0)
                return r;
        }
        has_name = true;
        bound = true;
        return 0;
    }

    long autobind() {
        for (int tries = 0; tries < 0x100000; tries++) {
            SockAddr sa;
            sa.data[0] = AF_UNIX;
            char hex[8];
            snprintf(hex, sizeof(hex), "%05x", g_autobind++ & 0xFFFFF);
            memcpy(sa.data + 3, hex, 5);
            sa.len = 8;
            if (!find_abstract(sa)) {
                name = sa;
                has_name = bound = true;
                return 0;
            }
        }
        return -EADDRINUSE;
    }

    long bind_path(const SockAddr& sa) {
        char path[PATH_MAX_LEN + 1];
        extract_path(sa, path);
        vfs::Inode* parent;
        const char* leaf;
        usize leaf_len;
        if (long r = vfs::resolve_parent(path, cwd(), &parent, &leaf, &leaf_len); r < 0)
            return r == -err::EXIST ? -EADDRINUSE : r;
        if (parent->lookup(leaf, leaf_len))
            return -EADDRINUSE;
        proc::Process* p = proc::current();
        u32 perms = 0777 & ~(p ? p->umask : 022);
        vfs::Inode* node = nullptr;
        if (parent->is_ramfs_dir()) {
            auto* n = new SockNode(perms);
            if (long r = static_cast<ramfs::Dir*>(parent)->attach(leaf, leaf_len, n); r < 0) {
                delete n;
                return r == -err::EXIST ? -EADDRINUSE : r;
            }
            node = n;
        } else if (long r = parent->create(leaf, leaf_len, vfs::S_IFSOCK | perms, nullptr, &node); r < 0) {
            return r == -err::EXIST ? -EADDRINUSE : r;
        }
        node_dev = node->dev;
        node_ino = node->ino;
        name = SockAddr{};
        name.data[0] = AF_UNIX;
        usize n = strlen(path);
        memcpy(name.data + 2, path, n);
        name.len = static_cast<u32>(2 + n + 1);
        return 0;
    }

    static UnixSocket* find_abstract(const SockAddr& sa) {
        for (UnixSocket* s = g_all; s; s = s->next_all)
            if (s->bound && !s->node_ino && s->name.len == sa.len && memcmp(s->name.data, sa.data, sa.len) == 0)
                return s;
        return nullptr;
    }

    // The bound socket an address names (g_unix held).
    long find_peer(const SockAddr& sa, UnixSocket** out) {
        if (sa.family() != AF_UNIX || sa.len <= 2)
            return -err::INVAL;
        UnixSocket* found = nullptr;
        if (sa.data[2] == 0) {
            found = find_abstract(sa);
        } else {
            char path[PATH_MAX_LEN + 1];
            extract_path(sa, path);
            vfs::Inode* node;
            if (long r = vfs::resolve(path, cwd(), true, &node); r < 0)
                return r;
            if ((node->mode & vfs::S_IFMT) != vfs::S_IFSOCK)
                return -ECONNREFUSED;
            for (UnixSocket* s = g_all; s && !found; s = s->next_all)
                if (s->bound && s->node_ino == node->ino && s->node_dev == node->dev)
                    found = s;
        }
        if (!found)
            return -ECONNREFUSED;
        if (found->type != type)
            return -EPROTOTYPE;
        *out = found;
        return 0;
    }

    long get_name(SockAddr* out, bool want_peer) override {
        LockGuard guard(g_unix);
        UnixSocket* s = this;
        if (want_peer) {
            if (!peer || (connection() && st != St::Connected))
                return -ENOTCONN;
            s = peer;
        }
        if (s->has_name) {
            *out = s->name;
        } else {
            *out = SockAddr{};
            out->data[0] = AF_UNIX;
            out->len = 2;
        }
        return 0;
    }

    // ---- connections ----

    long listen(int n) override {
        LockGuard guard(g_unix);
        if (!connection())
            return -EOPNOTSUPP_;
        if (!has_name || (st != St::Unconnected && st != St::Listening))
            return -err::INVAL;
        backlog = n < 0 ? 0 : n > 4096 ? 4096 : n;
        listen_cred = current_cred();
        st = St::Listening;
        return 0;
    }

    long connect_addr(const SockAddr& sa, bool nonblock) override {
        if (!connection())
            return connect_dgram(sa);
        for (;;) {
            u64 seen;
            {
                LockGuard guard(g_unix);
                if (st == St::Connected || st == St::Disconnected)
                    return -EISCONN;
                if (st == St::Listening)
                    return -err::INVAL;
                UnixSocket* l;
                if (long r = find_peer(sa, &l); r < 0)
                    return r;
                if (l->st != St::Listening)
                    return -ECONNREFUSED;
                if (l->nqueued <= l->backlog) {
                    join(l);
                    return 0;
                }
                if (nonblock)
                    return -err::AGAIN; // Linux reports EAGAIN for a full Unix backlog
                waiting_space = true;
                seen = snapshot();
            }
            if (long r = wait(seen, false, snd_timeout_ns); r < 0)
                return r == -err::AGAIN ? -ETIMEDOUT_ : r;
        }
    }

    // Create the server-side socket of a new connection and queue it on
    // the listener (g_unix held). The connection is usable right away.
    void join(UnixSocket* l) {
        auto* c = new UnixSocket(type);
        c->owned = false;
        c->st = St::Connected;
        c->peer = this;
        c->peer_cred = current_cred();
        c->has_peer_cred = true;
        c->passcred = l->passcred;
        c->has_name = l->has_name; // getsockname on the accepted socket names the listener
        c->name = l->name;
        peer = c;
        st = St::Connected;
        connected = true;
        peer_cred = l->listen_cred;
        has_peer_cred = true;
        UnixSocket** tailp = &l->acc_head;
        while (*tailp)
            tailp = &(*tailp)->acc_next;
        *tailp = c;
        l->nqueued++;
        kick(l);
    }

    long connect_dgram(const SockAddr& sa) {
        LockGuard guard(g_unix);
        if (sa.family() == AF_UNSPEC) {
            peer = nullptr;
            connected = false;
            return 0;
        }
        UnixSocket* t;
        if (long r = find_peer(sa, &t); r < 0)
            return r;
        peer = t;
        peer_dead = false;
        connected = true;
        return 0;
    }

    long accept(Socket** out, bool nonblock) override {
        for (;;) {
            u64 seen;
            {
                LockGuard guard(g_unix);
                if (st != St::Listening)
                    return -err::INVAL;
                if (UnixSocket* c = acc_head) {
                    acc_head = c->acc_next;
                    c->acc_next = nullptr;
                    c->owned = true;
                    c->connected = true;
                    nqueued--;
                    wake_space_waiters(); // clients blocked on a full backlog
                    *out = c;
                    return 0;
                }
                seen = snapshot();
            }
            if (long r = wait(seen, nonblock, rcv_timeout_ns); r < 0)
                return r;
        }
    }

    long shutdown(int how) override {
        LockGuard guard(g_unix);
        if (connection() && st != St::Connected && st != St::Disconnected)
            return -ENOTCONN;
        UnixSocket* p = connection() ? peer : nullptr;
        if (how == 0 || how == 2) {
            rcv_shut = true;
            if (p)
                p->snd_shut = true;
        }
        if (how == 1 || how == 2) {
            snd_shut = true;
            if (p)
                p->rcv_shut = true;
        }
        kick(this);
        if (p)
            kick(p);
        return 0;
    }

    // ---- data ----

    // A message holding `len` bytes, taking ownership of ctl's files.
    UnixMsg* make_msg(const u8* data, usize len, Control* ctl) {
        auto* m = static_cast<UnixMsg*>(kmalloc(sizeof(UnixMsg) + (len ? len : 1)));
        if (!m)
            return nullptr;
        m->next = nullptr;
        m->len = len;
        m->off = 0;
        m->from = SockAddr{};
        if (has_name)
            m->from = name;
        m->ctl = Control{};
        m->cred = current_cred();
        if (ctl) {
            m->ctl.files = ctl->files;
            m->ctl.nfiles = ctl->nfiles;
            ctl->files = nullptr;
            ctl->nfiles = 0;
            if (ctl->has_cred)
                m->cred = ctl->cred; // root may claim any credentials
        }
        memcpy(m->data, data, len);
        return m;
    }

    isize send_msg(const u8* data, usize len, int flags, const SockAddr* to, Control* ctl, bool nonblock) override {
        if (!connection())
            return send_dgram(data, len, to, ctl, nonblock);
        if (to && to->len)
            return st == St::Connected ? -EISCONN : -EOPNOTSUPP_;
        if (type == SOCK_SEQPACKET && len > QUEUE_LIMIT)
            return -EMSGSIZE;
        usize done = 0;
        for (;;) {
            u64 seen;
            {
                LockGuard guard(g_unix);
                if (snd_shut || st == St::Disconnected)
                    return done ? static_cast<isize>(done) : -err::PIPE;
                if (st != St::Connected || !peer)
                    return -ENOTCONN;
                UnixSocket* p = peer;
                if (p->rcv_shut)
                    return done ? static_cast<isize>(done) : -err::PIPE;
                usize room = p->qbytes < QUEUE_LIMIT ? QUEUE_LIMIT - p->qbytes : 0;
                usize n = 0;
                if (type == SOCK_SEQPACKET)
                    n = (room >= len || !p->head) ? len : 0; // records are never split
                else
                    n = min(len - done, room);
                if (n || (len == 0 && type == SOCK_SEQPACKET)) {
                    UnixMsg* m = make_msg(data + done, n, done == 0 ? ctl : nullptr);
                    if (!m)
                        return done ? static_cast<isize>(done) : -err::NOMEM;
                    p->enqueue(m);
                    done += n;
                }
                if (done == len)
                    return static_cast<isize>(done);
                seen = snapshot();
            }
            if (long r = wait(seen, nonblock, snd_timeout_ns); r < 0)
                return done ? static_cast<isize>(done) : r;
        }
    }

    isize send_dgram(const u8* data, usize len, const SockAddr* to, Control* ctl, bool nonblock) {
        if (len > DGRAM_MAX)
            return -EMSGSIZE;
        for (;;) {
            u64 seen;
            {
                LockGuard guard(g_unix);
                if (snd_shut)
                    return -err::PIPE;
                UnixSocket* t;
                if (to && to->len) {
                    if (long r = find_peer(*to, &t); r < 0)
                        return r;
                } else if (peer) {
                    t = peer;
                } else {
                    return peer_dead ? -ECONNREFUSED : -ENOTCONN;
                }
                if (t->peer && t->peer != this)
                    return -err::PERM; // it only listens to the socket it connected to
                if (t->rcv_shut)
                    return -err::PIPE;
                if (t->qcount < DGRAM_QUEUE_MAX && t->qbytes + len <= QUEUE_LIMIT) {
                    UnixMsg* m = make_msg(data, len, ctl);
                    if (!m)
                        return -err::NOMEM;
                    t->enqueue(m);
                    return static_cast<isize>(len);
                }
                if (nonblock)
                    return -err::AGAIN;
                waiting_space = true;
                seen = snapshot();
            }
            if (long r = wait(seen, false, snd_timeout_ns); r < 0)
                return r;
        }
    }

    // Hand a message's ancillary data to the reader. With MSG_PEEK the
    // files stay queued and the reader gets extra references.
    void take_control(UnixMsg* m, Control* got, bool peek) {
        got->has_cred = passcred;
        got->cred = m->cred;
        if (!m->ctl.nfiles)
            return;
        if (!peek) {
            got->files = m->ctl.files;
            got->nfiles = m->ctl.nfiles;
            m->ctl.files = nullptr;
            m->ctl.nfiles = 0;
            return;
        }
        got->files = static_cast<vfs::File**>(kmalloc(sizeof(vfs::File*) * m->ctl.nfiles));
        if (!got->files)
            return;
        for (int i = 0; i < m->ctl.nfiles; i++) {
            vfs::file_get(m->ctl.files[i]);
            got->files[i] = m->ctl.files[i];
        }
        got->nfiles = m->ctl.nfiles;
    }

    isize recv_msg(u8* buf, usize len, int flags, SockAddr* from, Control* ctl, bool nonblock,
                   usize* full_len) override {
        Control got;
        isize r = type == SOCK_STREAM ? recv_stream(buf, len, flags, from, &got, nonblock)
                                      : recv_record(buf, len, flags, from, &got, nonblock, full_len);
        if (ctl && r >= 0)
            *ctl = got;
        else
            release_control(got); // read() of a message with files: they are closed, as on Linux
        if (full_len && type == SOCK_STREAM)
            *full_len = r > 0 ? static_cast<usize>(r) : 0;
        return r;
    }

    // Is there nothing left to read, ever? (g_unix held)
    bool at_eof() const { return rcv_shut || st == St::Disconnected; }

    isize recv_stream(u8* buf, usize len, int flags, SockAddr* from, Control* got, bool nonblock) {
        bool peek = flags & MSG_PEEK;
        usize done = 0;
        if (from)
            from->len = 0;
        for (;;) {
            u64 seen;
            {
                Graveyard grave;
                LockGuard guard(g_unix);
                if (st == St::Listening)
                    return -err::INVAL;
                if (from && peer && peer->has_name)
                    *from = peer->name;
                usize consumed = 0;
                bool stop = false;
                UnixMsg* m = head;
                usize off = m ? m->off : 0;
                bool first = true;
                while (m && done < len && !stop) {
                    bool ancillary = m->ctl.nfiles > 0;
                    // Ancillary data and credential changes start a new read.
                    if (done && (ancillary || (passcred && m->cred.pid != got->cred.pid)))
                        break;
                    usize n = min(len - done, m->len - off);
                    memcpy(buf + done, m->data + off, n);
                    done += n;
                    if (first || ancillary)
                        take_control(m, got, peek);
                    first = false;
                    stop = ancillary; // don't merge past a message that carried files
                    if (peek) {
                        off += n;
                        if (off == m->len) {
                            m = m->next;
                            off = m ? m->off : 0;
                        }
                        continue;
                    }
                    m->off += n;
                    qbytes -= n;
                    consumed += n;
                    if (m->off == m->len) {
                        UnixMsg* gone = m;
                        m = m->next;
                        head = m;
                        if (!head)
                            tail = nullptr;
                        qcount--;
                        grave.add(gone);
                        off = m ? m->off : 0;
                    }
                }
                if (consumed)
                    room_freed();
                bool want_more = (flags & MSG_WAITALL) && !peek && done < len && !stop;
                if (done && !want_more)
                    return static_cast<isize>(done);
                if (error && !done) { // the peer closed with our data unread
                    long e = error;
                    error = 0;
                    return -e;
                }
                if (at_eof())
                    return static_cast<isize>(done);
                if (st != St::Connected)
                    return done ? static_cast<isize>(done) : -ENOTCONN;
                seen = snapshot();
            }
            if (long r = wait(seen, nonblock, rcv_timeout_ns); r < 0)
                return done ? static_cast<isize>(done) : r;
        }
    }

    isize recv_record(u8* buf, usize len, int flags, SockAddr* from, Control* got, bool nonblock,
                      usize* full_len) {
        for (;;) {
            u64 seen;
            {
                Graveyard grave;
                LockGuard guard(g_unix);
                if (st == St::Listening)
                    return -err::INVAL;
                if (UnixMsg* m = head) {
                    usize n = min(len, m->len);
                    memcpy(buf, m->data, n);
                    if (full_len)
                        *full_len = m->len;
                    if (from)
                        *from = connection() && peer ? (peer->has_name ? peer->name : SockAddr{}) : m->from;
                    take_control(m, got, flags & MSG_PEEK);
                    if (!(flags & MSG_PEEK)) {
                        grave.add(dequeue());
                        room_freed();
                    }
                    return static_cast<isize>(n);
                }
                if (connection()) {
                    if (at_eof())
                        return 0;
                    if (st != St::Connected)
                        return -ENOTCONN;
                } else if (rcv_shut) {
                    return 0;
                }
                seen = snapshot();
            }
            if (long r = wait(seen, nonblock, rcv_timeout_ns); r < 0)
                return r;
        }
    }

    // The inet-style entry points are never used for Unix sockets.
    isize send(const u8* data, usize len, int flags, const Endpoint*, bool nonblock) override {
        return send_msg(data, len, flags, nullptr, nullptr, nonblock);
    }
    isize recv(u8* buf, usize len, int flags, Endpoint*, bool nonblock, usize* full_len) override {
        return recv_msg(buf, len, flags, nullptr, nullptr, nonblock, full_len);
    }

    u16 poll_mask() override {
        using namespace ::poll;
        LockGuard guard(g_unix);
        if (st == St::Listening)
            return acc_head ? POLLIN : 0;
        u16 m = 0;
        if (head)
            m |= POLLIN;
        if (rcv_shut)
            m |= POLLIN | POLLRDHUP;
        if (error)
            m |= POLLERR;
        if (connection()) {
            if (st != St::Connected || (rcv_shut && snd_shut))
                m |= POLLHUP;
            if (st == St::Disconnected)
                m |= POLLIN | POLLRDHUP;
            bool writable = !peer || peer->qbytes < QUEUE_LIMIT;
            if (writable)
                m |= POLLOUT;
        } else if (!peer || (peer->qcount < DGRAM_QUEUE_MAX && peer->qbytes < QUEUE_LIMIT)) {
            m |= POLLOUT;
        }
        return m;
    }

    usize bytes_available() override {
        LockGuard guard(g_unix);
        if (type == SOCK_STREAM)
            return qbytes;
        return head ? head->len : 0;
    }

    // ---- options ----

    long set_option(int level, int name_, const u8* val, u32 len) override {
        if (level != 1) // SOL_SOCKET
            return OPT_UNHANDLED;
        if (name_ == 16) { // SO_PASSCRED
            i32 v = 0;
            memcpy(&v, val, min<u32>(len, 4));
            passcred = v != 0;
            return 0;
        }
        return OPT_UNHANDLED;
    }

    long get_option(int level, int name_, u8* val, u32* len) override {
        if (level != 1)
            return OPT_UNHANDLED;
        i32 v;
        switch (name_) {
        case 16: v = passcred; break;                 // SO_PASSCRED
        case 30: v = st == St::Listening; break;      // SO_ACCEPTCONN
        case 17: {                                    // SO_PEERCRED
            LockGuard guard(g_unix);
            Ucred c = has_peer_cred ? peer_cred : Ucred{0, ~0u, ~0u};
            memcpy(val, &c, sizeof(c));
            *len = sizeof(c);
            return 0;
        }
        case 31: return -ENOPROTOOPT;                 // SO_PEERSEC: no security modules
        default: return OPT_UNHANDLED;
        }
        memcpy(val, &v, 4);
        *len = 4;
        return 0;
    }

    // ---- teardown ----

    // Cut the connection to our peer (g_unix held). Unread data at our end
    // means the peer's data was lost: report a reset, like Linux.
    void disconnect_peer() {
        if (connection()) {
            if (UnixSocket* p = peer) {
                p->peer = nullptr;
                p->st = St::Disconnected;
                p->rcv_shut = p->snd_shut = true;
                if (head)
                    p->error = static_cast<int>(ECONNRESET);
                kick(p);
            }
        } else {
            for (UnixSocket* s = g_all; s; s = s->next_all)
                if (s->peer == this) {
                    s->peer = nullptr;
                    s->peer_dead = true;
                    kick(s);
                }
        }
        peer = nullptr;
    }

    // Detach from everything and hand the queue to `grave` (g_unix held).
    void destroy(Graveyard& grave) {
        UnixSocket** link = &g_all;
        while (*link && *link != this)
            link = &(*link)->next_all;
        if (*link)
            *link = next_all;
        while (UnixSocket* c = acc_head) { // never accepted: those connections die with us
            acc_head = c->acc_next;
            c->destroy(grave);
            delete c;
        }
        disconnect_peer();
        while (head)
            grave.add(dequeue());
    }

    void close() override {
        {
            Graveyard grave;
            LockGuard guard(g_unix);
            destroy(grave);
        }
        delete this;
    }
};

void kick(Socket* s) {
    auto* u = static_cast<UnixSocket*>(s);
    __atomic_add_fetch(&u->events, 1, __ATOMIC_RELEASE);
    s->wake();
}

// /proc/net/unix, in Linux's format (netstat -x, ss, lsof read it).
bool gen_proc_unix(genfs::TextBuffer& out, const void*) {
    out.printf("Num       RefCount Protocol Flags    Type St Inode Path\n");
    LockGuard guard(g_unix);
    for (UnixSocket* s = g_all; s; s = s->next_all) {
        int state = s->st == St::Connected ? 3 : s->st == St::Disconnected ? 4 : 1;
        u32 flags = s->st == St::Listening ? 0x10000 : 0; // __SO_ACCEPTCON
        out.printf("0000000000000000: 00000002 00000000 %08X %04X %02X %5lu", flags, s->type, state, s->ino);
        if (s->has_name && s->bound) {
            char path[PATH_MAX_LEN + 2];
            if (s->node_ino) {
                extract_path(s->name, path);
            } else { // abstract: shown with a leading '@'
                usize n = min<usize>(s->name.len - 3, PATH_MAX_LEN);
                path[0] = '@';
                for (usize i = 0; i < n; i++)
                    path[1 + i] = s->name.data[3 + i] ? static_cast<char>(s->name.data[3 + i]) : '@';
                path[1 + n] = 0;
            }
            out.printf(" %s", path);
        }
        out.printf("\n");
    }
    return true;
}

} // namespace

Socket* create_unix(int type) {
    LockGuard guard(g_unix);
    return new UnixSocket(type);
}

long unix_socketpair(int type, Socket** a, Socket** b) {
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_SEQPACKET)
        return -err::INVAL;
    LockGuard guard(g_unix);
    auto* x = new UnixSocket(type);
    auto* y = new UnixSocket(type);
    Ucred me = current_cred();
    x->peer = y;
    y->peer = x;
    x->st = y->st = St::Connected;
    x->connected = y->connected = true;
    x->peer_cred = y->peer_cred = me;
    x->has_peer_cred = y->has_peer_cred = true;
    *a = x;
    *b = y;
    return 0;
}

void unix_proc_init() { vfs::add_node("/proc/net/unix", new genfs::GeneratedFile(gen_proc_unix, nullptr)); }

} // namespace net
