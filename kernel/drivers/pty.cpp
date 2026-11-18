// Pseudo-terminals (Linux Unix98 PTYs): /dev/ptmx and /dev/pts/N.
//
// Terminal emulators (xterm, Konsole), ssh and script(1) open /dev/ptmx to
// get a master; the matching slave /dev/pts/N is the terminal the shell
// runs on. Bytes written to the master are typed input for the slave (with
// line editing, echo and Ctrl+C -> SIGINT, per the slave's termios); what
// programs write to the slave is read from the master.

#include <core/log.hpp>
#include <drivers/pty.hpp>
#include <fs/poll.hpp>
#include <fs/ramfs.hpp>
#include <fs/vfs.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <proc/process.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>
#include <sched/sync.hpp>

namespace pty {

namespace {

struct Termios {
    u32 c_iflag, c_oflag, c_cflag, c_lflag;
    u8 c_line;
    u8 c_cc[19];
};
static_assert(sizeof(Termios) == 36);
struct Termios2 {
    Termios base;
    u32 c_ispeed, c_ospeed;
};
struct WinSize {
    u16 ws_row, ws_col, ws_xpixel, ws_ypixel;
};

constexpr u32 ICRNL = 0000400, INLCR = 0000100, IGNCR = 0000200;
constexpr u32 OPOST = 0000001, ONLCR = 0000004;
constexpr u32 ISIG = 0000001, ICANON = 0000002, ECHO = 0000010, ECHOE = 0000020, ECHOK = 0000040, ECHONL = 0000100,
              ECHOCTL = 0001000, IEXTEN = 0100000;
constexpr int VINTR = 0, VQUIT = 1, VERASE = 2, VKILL = 3, VEOF = 4, VMIN = 6, VSUSP = 10, VWERASE = 14;

constexpr usize BUF = 8192;

struct Ring {
    char data[BUF];
    usize head = 0, count = 0;
    usize room() const { return BUF - count; }
    void push(char c) {
        if (count < BUF)
            data[(head + count++) % BUF] = c;
    }
    char pop() {
        char c = data[head];
        head = (head + 1) % BUF;
        count--;
        return c;
    }
};

class PtsNode;

// One master/slave pair.
struct Pty {
    int index;
    Termios termios;
    WinSize winsize{24, 80, 0, 0};
    int fg_pgrp = 0;
    bool locked = true;     // TIOCSPTLCK: slave can't be opened until unlocked
    Ring to_slave;          // cooked input ready for slave reads
    Ring to_master;         // slave output for master reads
    char line[BUF];
    usize line_len = 0;
    u32 eof_pending = 0;    // VEOF on an empty line: the next slave read returns 0
    u32 master_opens = 0, slave_opens = 0;
    bool slave_ever_opened = false; // master reads get EIO only after the slave was opened and closed
    bool master_gone = false;
    vfs::Inode* slave_node = nullptr; // the PtsNode
    sched::WaitQueue wq;    // its lock protects everything above
};

constexpr int MAX_PTYS = 64;
Pty* g_ptys[MAX_PTYS];
IrqSpinlock g_table_lock;

// ---- line discipline (wq lock held) ----

void out_char(Pty* p, char c) { // output processing towards the master
    if ((p->termios.c_oflag & OPOST) && (p->termios.c_oflag & ONLCR) && c == '\n')
        p->to_master.push('\r');
    p->to_master.push(c);
}

void echo_char(Pty* p, char c) {
    if (!(p->termios.c_lflag & ECHO))
        return;
    if ((p->termios.c_lflag & ECHOCTL) && static_cast<u8>(c) < 32 && c != '\n' && c != '\t') {
        out_char(p, '^');
        out_char(p, static_cast<char>(c + 64));
        return;
    }
    out_char(p, c);
}

// One byte typed on the master. Returns a signal to send to the
// foreground group (0 = none).
int input_char(Pty* p, char c) {
    const Termios& t = p->termios;
    if (t.c_iflag & IGNCR && c == '\r')
        return 0;
    if (t.c_iflag & ICRNL && c == '\r')
        c = '\n';
    else if (t.c_iflag & INLCR && c == '\n')
        c = '\r';
    if (t.c_lflag & ISIG) {
        int sig = c == static_cast<char>(t.c_cc[VINTR]) ? signal::SIGINT
                  : c == static_cast<char>(t.c_cc[VQUIT]) ? signal::SIGQUIT
                  : c == static_cast<char>(t.c_cc[VSUSP]) ? signal::SIGTSTP
                                                          : 0;
        if (sig) {
            echo_char(p, c);
            if (t.c_lflag & ICANON) {
                out_char(p, '\n');
                p->line_len = 0;
            }
            return sig;
        }
    }
    if (!(t.c_lflag & ICANON)) {
        p->to_slave.push(c);
        echo_char(p, c);
        return 0;
    }
    if (c == static_cast<char>(t.c_cc[VERASE]) || c == '\b') {
        if (p->line_len) {
            p->line_len--;
            if ((t.c_lflag & ECHO) && (t.c_lflag & ECHOE)) {
                out_char(p, '\b');
                out_char(p, ' ');
                out_char(p, '\b');
            }
        }
        return 0;
    }
    if (c == static_cast<char>(t.c_cc[VKILL])) {
        while (p->line_len) {
            p->line_len--;
            if ((t.c_lflag & ECHO) && (t.c_lflag & ECHOK)) {
                out_char(p, '\b');
                out_char(p, ' ');
                out_char(p, '\b');
            }
        }
        return 0;
    }
    if (c == static_cast<char>(t.c_cc[VWERASE]) && (t.c_lflag & IEXTEN)) {
        while (p->line_len && p->line[p->line_len - 1] == ' ')
            p->line_len--;
        while (p->line_len && p->line[p->line_len - 1] != ' ')
            p->line_len--;
        return 0;
    }
    if (c == static_cast<char>(t.c_cc[VEOF])) {
        if (!p->line_len)
            p->eof_pending++;
        for (usize i = 0; i < p->line_len; i++)
            p->to_slave.push(p->line[i]);
        p->line_len = 0;
        return 0;
    }
    if (c == '\n') {
        for (usize i = 0; i < p->line_len; i++)
            p->to_slave.push(p->line[i]);
        p->to_slave.push('\n');
        p->line_len = 0;
        if ((t.c_lflag & ECHO) || (t.c_lflag & ECHONL))
            out_char(p, '\n');
        return 0;
    }
    if (p->line_len < BUF - 1)
        p->line[p->line_len++] = c;
    echo_char(p, c);
    return 0;
}

bool slave_readable(Pty* p) {
    if (p->master_gone || p->eof_pending)
        return true;
    if (!(p->termios.c_lflag & ICANON))
        return p->to_slave.count >= max<usize>(1, p->termios.c_cc[VMIN]) ||
               (p->termios.c_cc[VMIN] == 0 && p->to_slave.count);
    // Canonical: a complete line (or EOF) must be there.
    for (usize i = 0; i < p->to_slave.count; i++)
        if (p->to_slave.data[(p->to_slave.head + i) % BUF] == '\n')
            return true;
    return false;
}

void wake(Pty* p) {
    p->wq.wake_all();
    poll::notify();
}

void destroy_if_unused(Pty* p);

// ---- the common ioctls (termios, window size, process groups) ----

long common_ioctl(Pty* p, u64 request, u64 arg, bool master) {
    constexpr u64 TCGETS = 0x5401, TCSETS = 0x5402, TCSETSW = 0x5403, TCSETSF = 0x5404, TCSBRK = 0x5409,
                  TCXONC = 0x540A, TCFLSH = 0x540B, TIOCSCTTY = 0x540E, TIOCGPGRP = 0x540F, TIOCSPGRP = 0x5410,
                  TIOCOUTQ = 0x5411, TIOCGWINSZ = 0x5413, TIOCSWINSZ = 0x5414, FIONREAD = 0x541B, TIOCPKT = 0x5420,
                  TIOCNOTTY = 0x5422, TIOCGSID = 0x5429, TCGETS2 = 0x802C542A, TCSETS2 = 0x402C542B,
                  TCSETSW2 = 0x402C542C, TCSETSF2 = 0x402C542D, FIONBIO = 0x5421;
    void* uarg = reinterpret_cast<void*>(arg);
    switch (request) {
    case TCGETS: {
        Termios t;
        {
            LockGuard guard(p->wq.lock());
            t = p->termios;
        }
        return uaccess::put(static_cast<Termios*>(uarg), t);
    }
    case TCGETS2: {
        Termios2 t{};
        {
            LockGuard guard(p->wq.lock());
            t.base = p->termios;
        }
        t.c_ispeed = t.c_ospeed = 38400;
        return uaccess::put(static_cast<Termios2*>(uarg), t);
    }
    case TCSETS: case TCSETSW: case TCSETSF: {
        Termios t;
        if (long r = uaccess::get(&t, static_cast<Termios*>(uarg)); r < 0)
            return r;
        LockGuard guard(p->wq.lock());
        p->termios = t;
        if (request == TCSETSF)
            p->to_slave.count = p->line_len = 0;
        return 0;
    }
    case TCSETS2: case TCSETSW2: case TCSETSF2: {
        Termios2 t;
        if (long r = uaccess::get(&t, static_cast<Termios2*>(uarg)); r < 0)
            return r;
        LockGuard guard(p->wq.lock());
        p->termios = t.base;
        if (request == TCSETSF2)
            p->to_slave.count = p->line_len = 0;
        return 0;
    }
    case TIOCGWINSZ: {
        WinSize w;
        {
            LockGuard guard(p->wq.lock());
            w = p->winsize;
        }
        return uaccess::put(static_cast<WinSize*>(uarg), w);
    }
    case TIOCSWINSZ: {
        WinSize w;
        if (long r = uaccess::get(&w, static_cast<WinSize*>(uarg)); r < 0)
            return r;
        int fg;
        bool changed;
        {
            LockGuard guard(p->wq.lock());
            changed = memcmp(&w, &p->winsize, sizeof(w)) != 0;
            p->winsize = w;
            fg = p->fg_pgrp;
        }
        if (changed && fg > 0)
            signal::send_to_group(fg, signal::SIGWINCH);
        return 0;
    }
    case TIOCGPGRP: {
        i32 g = p->fg_pgrp;
        return uaccess::put(static_cast<i32*>(uarg), g);
    }
    case TIOCSPGRP: {
        i32 g;
        if (long r = uaccess::get(&g, static_cast<i32*>(uarg)); r < 0)
            return r;
        p->fg_pgrp = g;
        return 0;
    }
    case TIOCGSID: {
        i32 sid = proc::current()->sid;
        return uaccess::put(static_cast<i32*>(uarg), sid);
    }
    case FIONREAD: {
        i32 n;
        {
            LockGuard guard(p->wq.lock());
            n = static_cast<i32>(master ? p->to_master.count : p->to_slave.count);
        }
        return uaccess::put(static_cast<i32*>(uarg), n);
    }
    case TIOCOUTQ: {
        i32 zero = 0;
        return uaccess::put(static_cast<i32*>(uarg), zero);
    }
    case TCFLSH: {
        LockGuard guard(p->wq.lock());
        if (arg == 0 || arg == 2) { // TCIFLUSH / TCIOFLUSH
            p->to_slave.count = 0;
            p->line_len = 0;
        }
        if (arg == 1 || arg == 2)
            p->to_master.count = 0;
        return 0;
    }
    case TIOCSCTTY: // become the controlling terminal: the caller's group goes to the foreground
        p->fg_pgrp = proc::current()->pgid;
        proc::current()->ctty = p->slave_node;
        return 0;
    case TIOCNOTTY: case TCSBRK: case TCXONC: case TIOCPKT: case FIONBIO:
        return 0;
    default:
        return -err::NOTTY;
    }
}

// ---- slave: /dev/pts/N ----

class PtsNode : public vfs::Inode {
public:
    explicit PtsNode(Pty* p) : m_pty(p) {
        ino = vfs::next_ino();
        mode = vfs::S_IFCHR | 0620;
        uid = proc::cred().ruid; // belongs to whoever opened /dev/ptmx (glibc's grantpt checks this)
        gid = vfs::GID_TTY;
        rdev = (136u << 8) | static_cast<u32>(p->index); // Linux: majors 136+ for pts
        atime = mtime = ctime = vfs::now();
    }

    bool seekable() const override { return false; }

    void on_open(vfs::File* f) override {
        Pty* p = m_pty;
        proc::Process* me = proc::current();
        LockGuard guard(p->wq.lock());
        if (p->slave_opens++ == 0 && p->fg_pgrp == 0)
            p->fg_pgrp = me->pgid;
        // Linux: a session leader opening a terminal without O_NOCTTY makes it
        // its controlling terminal, with the leader's group in the foreground
        // (what xterm's child does after setsid(); shells check tcgetpgrp()).
        if (me->pid == me->sid && !(f->flags & vfs::O_NOCTTY) && !me->ctty) {
            p->fg_pgrp = me->pgid;
            me->ctty = this;
        }
        p->slave_ever_opened = true;
    }

    void on_release(vfs::File*) override {
        Pty* p = m_pty;
        {
            LockGuard guard(p->wq.lock());
            p->slave_opens--;
        }
        wake(p); // the master sees EIO/HUP once nobody has the slave open
        destroy_if_unused(p);
    }

    isize read_file(vfs::File* f, u64, void* buf, usize count) override {
        Pty* p = m_pty;
        if (count == 0)
            return 0;
        char tmp[512];
        for (;;) {
            usize n = 0;
            bool eof = false;
            {
                LockGuard guard(p->wq.lock());
                if (slave_readable(p)) {
                    bool canonical = p->termios.c_lflag & ICANON;
                    while (n < count && n < sizeof(tmp) && p->to_slave.count) {
                        char c = p->to_slave.pop();
                        tmp[n++] = c;
                        if (canonical && c == '\n')
                            break;
                    }
                    if (!n && p->eof_pending) {
                        p->eof_pending--;
                        eof = true;
                    } else if (!n && p->master_gone) {
                        eof = true;
                    }
                } else if (!(p->termios.c_lflag & ICANON) && p->termios.c_cc[VMIN] == 0) {
                    eof = true; // VMIN=0, VTIME=0: non-blocking read, nothing there
                }
            }
            if (n) {
                memcpy(buf, tmp, n);
                wake(p);
                return static_cast<isize>(n);
            }
            if (eof)
                return 0;
            if (f->flags & vfs::O_NONBLOCK)
                return -err::AGAIN;
            if (signal::interrupted())
                return -err::INTR;
            p->wq.wait_until([&] { return slave_readable(p) || signal::interrupted(); });
        }
    }

    isize write_file(vfs::File* f, u64, const void* buf, usize count) override {
        Pty* p = m_pty;
        auto* in = static_cast<const char*>(buf);
        usize done = 0;
        char tmp[512];
        while (done < count) {
            usize chunk = min<usize>(count - done, sizeof(tmp));
            memcpy(tmp, in + done, chunk);
            usize used = 0;
            {
                LockGuard guard(p->wq.lock());
                if (p->master_gone)
                    return done ? static_cast<isize>(done) : -err::IO;
                while (used < chunk && p->to_master.room() >= 2)
                    out_char(p, tmp[used++]);
            }
            if (used) {
                done += used;
                wake(p);
                continue;
            }
            if (f->flags & vfs::O_NONBLOCK)
                return done ? static_cast<isize>(done) : -err::AGAIN;
            if (signal::interrupted())
                return done ? static_cast<isize>(done) : -err::INTR;
            p->wq.wait_until([&] { return p->to_master.room() >= 2 || p->master_gone || signal::interrupted(); });
        }
        return static_cast<isize>(done);
    }

    u16 poll(vfs::File*) override {
        Pty* p = m_pty;
        LockGuard guard(p->wq.lock());
        u16 m = 0;
        if (slave_readable(p))
            m |= poll::POLLIN;
        if (p->to_master.room() >= 2 && !p->master_gone)
            m |= poll::POLLOUT;
        if (p->master_gone)
            m |= poll::POLLHUP;
        return m;
    }

    long ioctl_file(vfs::File*, u64 request, u64 arg) override { return common_ioctl(m_pty, request, arg, false); }

    Pty* pty() const { return m_pty; }

private:
    Pty* m_pty;
};

void destroy_if_unused(Pty* p) {
    {
        LockGuard guard(p->wq.lock());
        if (p->master_opens || p->slave_opens)
            return;
    }
    char path[32];
    snprintf(path, sizeof(path), "/dev/pts/%d", p->index);
    vfs::remove_node(path);
    {
        LockGuard guard(g_table_lock);
        g_ptys[p->index] = nullptr;
    }
    // The node may still be referenced by a path lookup in progress; it's
    // tiny, so it is kept (pointing at a dead pair would be worse).
    delete p;
}

// ---- master: /dev/ptmx (every open creates a new pair) ----

Pty* pty_of(vfs::File* f) { return static_cast<Pty*>(f->private_data); }

class PtmxNode : public vfs::Inode {
public:
    PtmxNode() {
        ino = vfs::next_ino();
        mode = vfs::S_IFCHR | 0666;
        rdev = (5u << 8) | 2; // Linux: 5,2
        atime = mtime = ctime = vfs::now();
    }

    bool seekable() const override { return false; }

    void on_open(vfs::File* f) override {
        auto* p = new Pty;
        p->termios = Termios{ICRNL, OPOST | ONLCR, 0000017 | 0000060 | 0000200,
                             ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | IEXTEN, 0,
                             {3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22, 0, 0, 0}};
        p->master_opens = 1;
        p->index = -1;
        {
            LockGuard guard(g_table_lock);
            for (int i = 0; i < MAX_PTYS; i++) {
                if (!g_ptys[i]) {
                    g_ptys[i] = p;
                    p->index = i;
                    break;
                }
            }
        }
        f->private_data = p;
        if (p->index < 0) {
            klog::warn("pty: all %d pseudo-terminals in use", MAX_PTYS);
            return; // reads/writes on this master fail
        }
        p->slave_node = new PtsNode(p);
        char path[32];
        snprintf(path, sizeof(path), "/dev/pts/%d", p->index);
        vfs::add_node(path, p->slave_node);
    }

    void on_release(vfs::File* f) override {
        Pty* p = pty_of(f);
        if (!p)
            return;
        int fg;
        {
            LockGuard guard(p->wq.lock());
            p->master_opens = 0;
            p->master_gone = true;
            fg = p->fg_pgrp;
        }
        wake(p);
        if (fg > 0)
            signal::send_to_group(fg, signal::SIGHUP); // the terminal window closed
        if (p->index < 0) {
            delete p;
            return;
        }
        destroy_if_unused(p);
    }

    isize read_file(vfs::File* f, u64, void* buf, usize count) override {
        Pty* p = pty_of(f);
        if (!p || p->index < 0)
            return -err::IO;
        char tmp[1024];
        for (;;) {
            usize n = 0;
            bool hangup = false;
            {
                LockGuard guard(p->wq.lock());
                while (n < count && n < sizeof(tmp) && p->to_master.count)
                    tmp[n++] = p->to_master.pop();
                // Slave closed after having been opened: EIO, as Linux (xterm exits on it).
                hangup = !n && p->slave_opens == 0 && !p->locked && p->slave_ever_opened;
            }
            if (n) {
                memcpy(buf, tmp, n);
                wake(p);
                return static_cast<isize>(n);
            }
            if (hangup)
                return -err::IO;
            if (f->flags & vfs::O_NONBLOCK)
                return -err::AGAIN;
            if (signal::interrupted())
                return -err::INTR;
            p->wq.wait_until([&] {
                return p->to_master.count || (p->slave_opens == 0 && p->slave_ever_opened) || signal::interrupted();
            });
        }
    }

    isize write_file(vfs::File* f, u64, const void* buf, usize count) override {
        Pty* p = pty_of(f);
        if (!p || p->index < 0)
            return -err::IO;
        char tmp[1024];
        usize done = 0;
        while (done < count) {
            usize chunk = min<usize>(count - done, sizeof(tmp));
            memcpy(tmp, static_cast<const char*>(buf) + done, chunk);
            int signals[8];
            int nsig = 0;
            int fg;
            {
                LockGuard guard(p->wq.lock());
                for (usize i = 0; i < chunk; i++) {
                    int s = input_char(p, tmp[i]);
                    if (s && nsig < 8)
                        signals[nsig++] = s;
                }
                fg = p->fg_pgrp;
            }
            for (int i = 0; i < nsig; i++)
                if (fg > 0)
                    signal::send_to_group(fg, signals[i]);
            done += chunk;
        }
        wake(p);
        return static_cast<isize>(count);
    }

    u16 poll(vfs::File* f) override {
        Pty* p = pty_of(f);
        if (!p || p->index < 0)
            return poll::POLLERR;
        LockGuard guard(p->wq.lock());
        u16 m = poll::POLLOUT;
        if (p->to_master.count)
            m |= poll::POLLIN;
        if (p->slave_opens == 0 && p->slave_ever_opened)
            m |= poll::POLLHUP | poll::POLLIN;
        return m;
    }

    long ioctl_file(vfs::File* f, u64 request, u64 arg) override {
        constexpr u64 TIOCGPTN = 0x80045430, TIOCSPTLCK = 0x40045431, TIOCGPTLCK = 0x80045439, TIOCGPTPEER = 0x5441;
        Pty* p = pty_of(f);
        if (!p || p->index < 0)
            return -err::IO;
        void* uarg = reinterpret_cast<void*>(arg);
        switch (request) {
        case TIOCGPTN: {
            u32 n = static_cast<u32>(p->index);
            return uaccess::put(static_cast<u32*>(uarg), n);
        }
        case TIOCSPTLCK: {
            i32 lock;
            if (long r = uaccess::get(&lock, static_cast<i32*>(uarg)); r < 0)
                return r;
            p->locked = lock != 0;
            return 0;
        }
        case TIOCGPTLCK: {
            i32 lock = p->locked;
            return uaccess::put(static_cast<i32*>(uarg), lock);
        }
        case TIOCGPTPEER: { // open the slave directly (no path lookup)
            u32 flags = static_cast<u32>(arg) & (vfs::O_ACCMODE | vfs::O_NONBLOCK | vfs::O_CLOEXEC | vfs::O_NOCTTY);
            vfs::File* s = vfs::open_inode(p->slave_node, flags & ~(vfs::O_CLOEXEC | vfs::O_NOCTTY));
            char path[32];
            snprintf(path, sizeof(path), "/dev/pts/%d", p->index);
            vfs::file_set_path(s, path);
            long fd = proc::fd_install(proc::current(), s, 0, flags & vfs::O_CLOEXEC);
            if (fd < 0)
                vfs::file_put(s);
            return fd;
        }
        default:
            return common_ioctl(p, request, arg, true);
        }
    }
};

} // namespace

void init() {
    vfs::mkdir_p("/dev/pts", 0755);
    vfs::add_node("/dev/ptmx", new PtmxNode);
}

} // namespace pty
