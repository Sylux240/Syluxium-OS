// Event file descriptors, as on Linux: epoll, eventfd, timerfd, signalfd.
//
// Modern Linux programs build their main loops on these (GLib/GTK, Qt,
// Wayland, D-Bus, systemd-style daemons, Firefox): one epoll set watching
// sockets, an eventfd for cross-thread wakeups, a timerfd for timeouts and
// a signalfd instead of signal handlers.
//
// Readiness uses the kernel's poll model: every source of events calls
// poll::notify(), and waiters re-check the files they care about.

#include <core/log.hpp>
#include <fs/poll.hpp>
#include <fs/vfs.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <proc/process.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>
#include <sched/sched.hpp>
#include <sched/sync.hpp>
#include <syscall/syscall.hpp>
#include <time/clock.hpp>
#include <time/rtc.hpp>

namespace events {

namespace {

constexpr u32 O_NONBLOCK_FLAG = 04000, O_CLOEXEC_FLAG = 02000000;
constexpr u64 WAIT_FOREVER = ~0ull;

// Base for the "anonymous inode" files below (no name in any directory).
class AnonInode : public vfs::Inode {
public:
    AnonInode() {
        ino = vfs::next_ino();
        mode = 0600;
        atime = mtime = ctime = vfs::now();
    }
    bool seekable() const override { return false; }
};

long install(vfs::Inode* inode, u32 flags, const char* name) {
    vfs::File* f = vfs::open_inode(inode, vfs::O_RDWR | ((flags & O_NONBLOCK_FLAG) ? vfs::O_NONBLOCK : 0));
    char path[48];
    snprintf(path, sizeof(path), "anon_inode:[%s]", name);
    vfs::file_set_path(f, path);
    long fd = proc::fd_install(proc::current(), f, 0, flags & O_CLOEXEC_FLAG);
    if (fd < 0)
        vfs::file_put(f);
    return fd;
}

// ======================= eventfd =======================

constexpr u32 EFD_SEMAPHORE = 1;
constexpr u64 EVENTFD_MAX = ~0ull - 1;

class EventFd : public AnonInode {
public:
    EventFd(u64 initial, bool semaphore) : m_count(initial), m_semaphore(semaphore) {}

    isize read_file(vfs::File* f, u64, void* buf, usize count) override {
        if (count < 8)
            return -err::INVAL;
        for (;;) {
            u64 value = 0;
            {
                LockGuard guard(m_wq.lock());
                if (m_count) {
                    value = m_semaphore ? 1 : m_count;
                    m_count -= value;
                }
            }
            if (value) {
                m_wq.wake_all();
                poll::notify();
                memcpy(buf, &value, 8);
                return 8;
            }
            if (f->flags & vfs::O_NONBLOCK)
                return -err::AGAIN;
            if (signal::interrupted())
                return -err::INTR;
            m_wq.wait_until([&] { return m_count > 0 || signal::interrupted(); });
        }
    }

    isize write_file(vfs::File* f, u64, const void* buf, usize count) override {
        if (count < 8)
            return -err::INVAL;
        u64 value;
        memcpy(&value, buf, 8);
        if (value == ~0ull)
            return -err::INVAL;
        for (;;) {
            bool done = false;
            {
                LockGuard guard(m_wq.lock());
                if (EVENTFD_MAX - m_count >= value) {
                    m_count += value;
                    done = true;
                }
            }
            if (done) {
                m_wq.wake_all();
                poll::notify();
                return 8;
            }
            if (f->flags & vfs::O_NONBLOCK)
                return -err::AGAIN;
            if (signal::interrupted())
                return -err::INTR;
            m_wq.wait_until([&] { return EVENTFD_MAX - m_count >= value || signal::interrupted(); });
        }
    }

    u16 poll(vfs::File*) override {
        LockGuard guard(m_wq.lock());
        return static_cast<u16>((m_count ? poll::POLLIN : 0) | (m_count < EVENTFD_MAX ? poll::POLLOUT : 0));
    }

    void on_release(vfs::File*) override { delete this; }

private:
    u64 m_count;
    bool m_semaphore;
    sched::WaitQueue m_wq; // its lock protects m_count
};

// ======================= timerfd =======================

constexpr int CLOCK_REALTIME = 0, CLOCK_MONOTONIC = 1, CLOCK_BOOTTIME = 7, CLOCK_REALTIME_ALARM = 8,
              CLOCK_BOOTTIME_ALARM = 9;
constexpr u32 TFD_TIMER_ABSTIME = 1;

struct Timespec64 {
    i64 sec, nsec;
};
struct ITimerSpec {
    Timespec64 interval, value;
};

u64 to_ns(const Timespec64& t) { return static_cast<u64>(t.sec) * 1'000'000'000ull + static_cast<u64>(t.nsec); }
Timespec64 from_ns(u64 ns) { return Timespec64{static_cast<i64>(ns / 1'000'000'000ull), static_cast<i64>(ns % 1'000'000'000ull)}; }

class TimerFd;
IrqSpinlock g_timers_lock;
TimerFd* g_timers = nullptr;       // every timerfd, for the expiry thread
sched::WaitQueue g_timer_thread_wq;
volatile u64 g_timers_changed = 0;
bool g_timer_thread_started = false;

class TimerFd : public AnonInode {
public:
    explicit TimerFd(int clock) : m_realtime(clock == CLOCK_REALTIME || clock == CLOCK_REALTIME_ALARM) {
        LockGuard guard(g_timers_lock);
        next = g_timers;
        g_timers = this;
    }

    TimerFd* next = nullptr;

    u64 now() const { return m_realtime ? rtc::realtime_ns() : clock::now_ns(); }

    // Fold elapsed expirations into the counter (lock held).
    void update() {
        if (!m_deadline)
            return;
        u64 t = now();
        if (t < m_deadline)
            return;
        u64 n = 1;
        if (m_interval) {
            n += (t - m_deadline) / m_interval;
            m_deadline += n * m_interval;
        } else {
            m_deadline = 0;
        }
        m_expirations += n;
    }

    // Monotonic nanoseconds until the next expiry (WAIT_FOREVER if disarmed).
    u64 remaining_ns() {
        LockGuard guard(m_wq.lock());
        if (!m_deadline)
            return WAIT_FOREVER;
        u64 t = now();
        return m_deadline > t ? m_deadline - t : 0;
    }

    // Called by the expiry thread: publish expirations, wake readers/pollers.
    bool fire_if_due() {
        bool fired = false;
        {
            LockGuard guard(m_wq.lock());
            u64 before = m_expirations;
            update();
            fired = m_expirations != before;
        }
        if (fired)
            m_wq.wake_all();
        return fired;
    }

    long settime(u32 flags, const ITimerSpec& spec, ITimerSpec* old) {
        {
            LockGuard guard(m_wq.lock());
            update();
            if (old)
                *old = current_locked();
            m_interval = to_ns(spec.interval);
            u64 value = to_ns(spec.value);
            if (!value) {
                m_deadline = 0;
            } else {
                m_deadline = (flags & TFD_TIMER_ABSTIME) ? value : now() + value;
                if (!m_deadline)
                    m_deadline = 1;
            }
            m_expirations = 0;
        }
        __atomic_add_fetch(&g_timers_changed, 1, __ATOMIC_SEQ_CST);
        g_timer_thread_wq.wake_all();
        return 0;
    }

    ITimerSpec gettime() {
        LockGuard guard(m_wq.lock());
        update();
        return current_locked();
    }

    isize read_file(vfs::File* f, u64, void* buf, usize count) override {
        if (count < 8)
            return -err::INVAL;
        for (;;) {
            u64 n = 0;
            {
                LockGuard guard(m_wq.lock());
                update();
                n = m_expirations;
                m_expirations = 0;
            }
            if (n) {
                memcpy(buf, &n, 8);
                return 8;
            }
            if (f->flags & vfs::O_NONBLOCK)
                return -err::AGAIN;
            if (signal::interrupted())
                return -err::INTR;
            u64 wait = remaining_ns();
            auto ready = [&] { return m_expirations > 0 || signal::interrupted(); };
            if (wait == WAIT_FOREVER)
                m_wq.wait_until(ready);
            else
                m_wq.wait_until_timeout(ready, wait ? wait : 1);
        }
    }

    u16 poll(vfs::File*) override {
        LockGuard guard(m_wq.lock());
        update();
        return m_expirations ? poll::POLLIN : 0;
    }

    void on_release(vfs::File*) override {
        {
            LockGuard guard(g_timers_lock);
            TimerFd** link = &g_timers;
            while (*link && *link != this)
                link = &(*link)->next;
            if (*link)
                *link = next;
        }
        delete this;
    }

private:
    ITimerSpec current_locked() {
        ITimerSpec s{};
        s.interval = from_ns(m_interval);
        if (m_deadline) {
            u64 t = now();
            s.value = from_ns(m_deadline > t ? m_deadline - t : 1);
        }
        return s;
    }

    bool m_realtime;
    u64 m_deadline = 0;  // absolute, in this timer's clock; 0 = disarmed
    u64 m_interval = 0;
    u64 m_expirations = 0;
    sched::WaitQueue m_wq;
};

// Sleeps until the earliest armed timerfd is due, then wakes its pollers.
void timer_thread(void*) {
    for (;;) {
        u64 seen = __atomic_load_n(&g_timers_changed, __ATOMIC_SEQ_CST);
        u64 nearest = WAIT_FOREVER;
        bool any_fired = false;
        {
            LockGuard guard(g_timers_lock);
            for (TimerFd* t = g_timers; t; t = t->next) {
                any_fired |= t->fire_if_due();
                u64 r = t->remaining_ns();
                if (r < nearest)
                    nearest = r;
            }
        }
        if (any_fired)
            poll::notify();
        auto changed = [&] { return __atomic_load_n(&g_timers_changed, __ATOMIC_SEQ_CST) != seen; };
        if (nearest == WAIT_FOREVER)
            g_timer_thread_wq.wait_until(changed);
        else
            g_timer_thread_wq.wait_until_timeout(changed, nearest ? nearest : 100'000);
    }
}

// ======================= signalfd =======================

struct PACKED SignalfdSiginfo {
    u32 ssi_signo;
    i32 ssi_errno, ssi_code;
    u32 ssi_pid, ssi_uid;
    i32 ssi_fd;
    u32 ssi_tid, ssi_band, ssi_overrun, ssi_trapno;
    i32 ssi_status, ssi_int;
    u64 ssi_ptr, ssi_utime, ssi_stime, ssi_addr;
    u16 ssi_addr_lsb;
    u16 pad2;
    i32 ssi_syscall;
    u64 ssi_call_addr;
    u32 ssi_arch;
    u8 pad[28];
};
static_assert(sizeof(SignalfdSiginfo) == 128);

constexpr u64 UNBLOCKABLE = (1ull << 8) | (1ull << 18); // SIGKILL, SIGSTOP

class SignalFd : public AnonInode {
public:
    explicit SignalFd(u64 mask) : mask(mask & ~UNBLOCKABLE) {}
    u64 mask;

    isize read_file(vfs::File* f, u64, void* buf, usize count) override {
        if (count < sizeof(SignalfdSiginfo))
            return -err::INVAL;
        usize max = count / sizeof(SignalfdSiginfo), n = 0;
        for (;;) {
            while (n < max) {
                int sig = signal::dequeue(mask);
                if (!sig)
                    break;
                SignalfdSiginfo info{};
                info.ssi_signo = static_cast<u32>(sig);
                info.ssi_code = 0; // SI_USER
                memcpy(static_cast<u8*>(buf) + n * sizeof(info), &info, sizeof(info));
                n++;
            }
            if (n)
                return static_cast<isize>(n * sizeof(SignalfdSiginfo));
            if (f->flags & vfs::O_NONBLOCK)
                return -err::AGAIN;
            if (signal::interrupted())
                return -err::INTR;
            u64 seq = poll::sequence();
            if (signal::pending_set() & mask)
                continue;
            poll::wait_for_change(seq, WAIT_FOREVER);
        }
    }

    u16 poll(vfs::File*) override { return (signal::pending_set() & mask) ? poll::POLLIN : 0; }
    void on_release(vfs::File*) override { delete this; }
};

// ======================= epoll =======================

constexpr u32 EPOLLIN = 0x001, EPOLLOUT = 0x004, EPOLLERR = 0x008, EPOLLHUP = 0x010, EPOLLRDNORM = 0x040,
              EPOLLWRNORM = 0x100, EPOLLEXCLUSIVE = 1u << 28, EPOLLWAKEUP = 1u << 29, EPOLLONESHOT = 1u << 30,
              EPOLLET = 1u << 31;
constexpr int EPOLL_CTL_ADD = 1, EPOLL_CTL_DEL = 2, EPOLL_CTL_MOD = 3;

struct PACKED EpollEvent {
    u32 events;
    u64 data;
};
static_assert(sizeof(EpollEvent) == 12);

struct EpItem {
    int fd;
    vfs::File* file;  // not referenced: closing the file removes the item
    u32 events;
    u64 data;
    u64 reported_seq; // poll sequence when last reported (edge-triggered mode)
    bool reported;
    bool disabled;    // EPOLLONESHOT fired
    EpItem* next;
};

class Epoll;
// List of all epoll sets. A sleeping mutex: forgetting a closed file takes
// each set's own mutex while the list is held (lock order: list, then set).
sched::Mutex g_epolls_lock;
Epoll* g_epolls = nullptr;

class Epoll : public AnonInode {
public:
    Epoll() {
        LockGuard guard(g_epolls_lock);
        next_epoll = g_epolls;
        g_epolls = this;
    }

    Epoll* next_epoll = nullptr;

    long ctl(int op, int fd, vfs::File* file, const EpollEvent* ev) {
        LockGuard guard(m_lock);
        EpItem** link = &m_items;
        while (*link && !((*link)->fd == fd && (*link)->file == file))
            link = &(*link)->next;
        EpItem* item = *link;
        switch (op) {
        case EPOLL_CTL_ADD: {
            if (item)
                return -err::EXIST;
            auto* n = new EpItem{fd, file, ev->events | EPOLLERR | EPOLLHUP, ev->data, 0, false, false, m_items};
            m_items = n;
            break;
        }
        case EPOLL_CTL_MOD:
            if (!item)
                return -err::NOENT;
            item->events = ev->events | EPOLLERR | EPOLLHUP;
            item->data = ev->data;
            item->disabled = false;
            item->reported = false;
            break;
        case EPOLL_CTL_DEL:
            if (!item)
                return -err::NOENT;
            *link = item->next;
            delete item;
            break;
        default:
            return -err::INVAL;
        }
        poll::notify(); // waiters may now have something (or less) to report
        return 0;
    }

    // Fill `out` with ready items (consume == false: just count them).
    int collect(EpollEvent* out, int max, bool consume) {
        LockGuard guard(m_lock);
        u64 seq = poll::sequence();
        int n = 0;
        for (EpItem* it = m_items; it && n < max; it = it->next) {
            if (it->disabled)
                continue;
            u32 mask = it->file->inode->poll(it->file);
            if (mask & EPOLLIN) mask |= EPOLLRDNORM;
            if (mask & EPOLLOUT) mask |= EPOLLWRNORM;
            u32 ready = mask & it->events & ~(EPOLLET | EPOLLONESHOT | EPOLLEXCLUSIVE | EPOLLWAKEUP);
            if (!ready)
                continue;
            // Edge-triggered: report again only after some new event happened.
            if ((it->events & EPOLLET) && it->reported && it->reported_seq == seq)
                continue;
            if (consume) {
                out[n] = EpollEvent{ready, it->data};
                it->reported = true;
                it->reported_seq = seq;
                if (it->events & EPOLLONESHOT)
                    it->disabled = true;
            }
            n++;
        }
        return n;
    }

    long wait(EpollEvent* out, int max, u64 timeout_ns) {
        u64 deadline = timeout_ns == WAIT_FOREVER ? WAIT_FOREVER : clock::now_ns() + timeout_ns;
        for (;;) {
            u64 seq = poll::sequence();
            int n = collect(out, max, true);
            if (n || timeout_ns == 0)
                return n;
            if (signal::interrupted())
                return -err::INTR;
            u64 now = clock::now_ns();
            if (deadline != WAIT_FOREVER && now >= deadline)
                return 0;
            poll::wait_for_change(seq, deadline == WAIT_FOREVER ? WAIT_FOREVER : deadline - now);
        }
    }

    // An epoll fd is itself pollable (nested epoll sets, poll() on it).
    u16 poll(vfs::File*) override {
        EpollEvent dummy;
        return collect(&dummy, 1, false) ? poll::POLLIN : 0;
    }

    // A watched file was closed for good: forget it.
    void forget(vfs::File* f) {
        LockGuard guard(m_lock);
        EpItem** link = &m_items;
        while (*link) {
            if ((*link)->file == f) {
                EpItem* dead = *link;
                *link = dead->next;
                delete dead;
            } else {
                link = &(*link)->next;
            }
        }
    }

    void on_release(vfs::File*) override {
        {
            LockGuard guard(g_epolls_lock);
            Epoll** link = &g_epolls;
            while (*link && *link != this)
                link = &(*link)->next_epoll;
            if (*link)
                *link = next_epoll;
        }
        while (EpItem* it = m_items) {
            m_items = it->next;
            delete it;
        }
        delete this;
    }

private:
    sched::Mutex m_lock;
    EpItem* m_items = nullptr;
};

// vfs calls this before a file's last reference goes away (the file is
// still valid). Holding the list lock keeps every set alive meanwhile.
void file_released(vfs::File* f) {
    LockGuard guard(g_epolls_lock);
    for (Epoll* e = g_epolls; e; e = e->next_epoll)
        e->forget(f);
}

// ======================= system calls =======================

enum AnonKind : u8 { KIND_NONE, KIND_EVENTFD, KIND_TIMERFD, KIND_SIGNALFD, KIND_EPOLL };

// Look up `fd` and check it is an event file of the given kind. On success
// *out holds a reference the caller must drop with file_put.
template <typename T>
T* lookup(long fd, AnonKind kind, vfs::File** out) {
    vfs::File* f = fd >= 0 ? proc::fd_get(proc::current(), static_cast<int>(fd)) : nullptr;
    if (!f)
        return nullptr;
    if (f->inode->anon_kind != kind) {
        vfs::file_put(f);
        return nullptr;
    }
    *out = f;
    return static_cast<T*>(f->inode);
}

class FileRef {
public:
    ~FileRef() {
        if (f)
            vfs::file_put(f);
    }
    vfs::File* f = nullptr;
};

long sys_eventfd2(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a1 & ~static_cast<u64>(O_NONBLOCK_FLAG | O_CLOEXEC_FLAG | EFD_SEMAPHORE))
        return -err::INVAL;
    auto* e = new EventFd(static_cast<u32>(a0), a1 & EFD_SEMAPHORE);
    e->anon_kind = KIND_EVENTFD;
    return install(e, static_cast<u32>(a1), "eventfd");
}

long sys_eventfd(SYSCALL_ARGS) { return sys_eventfd2(a0, 0, a2, a3, a4, a5, frame); }

long sys_timerfd_create(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    int c = static_cast<int>(a0);
    if (c != CLOCK_REALTIME && c != CLOCK_MONOTONIC && c != CLOCK_BOOTTIME && c != CLOCK_REALTIME_ALARM &&
        c != CLOCK_BOOTTIME_ALARM)
        return -err::INVAL;
    if (a1 & ~static_cast<u64>(O_NONBLOCK_FLAG | O_CLOEXEC_FLAG))
        return -err::INVAL;
    if (!__atomic_exchange_n(&g_timer_thread_started, true, __ATOMIC_ACQ_REL))
        sched::spawn("timerfd", timer_thread, nullptr, -1, true);
    auto* t = new TimerFd(c);
    t->anon_kind = KIND_TIMERFD;
    return install(t, static_cast<u32>(a1), "timerfd");
}

long sys_timerfd_settime(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    FileRef ref;
    auto* t = lookup<TimerFd>(static_cast<int>(a0), KIND_TIMERFD, &ref.f);
    if (!t)
        return -err::INVAL;
    ITimerSpec spec;
    if (long r = uaccess::get(&spec, reinterpret_cast<ITimerSpec*>(a2)); r < 0)
        return r;
    if (spec.value.nsec < 0 || spec.value.nsec >= 1'000'000'000 || spec.interval.nsec < 0 ||
        spec.interval.nsec >= 1'000'000'000 || spec.value.sec < 0 || spec.interval.sec < 0)
        return -err::INVAL;
    ITimerSpec old;
    t->settime(static_cast<u32>(a1), spec, &old);
    if (a3)
        return uaccess::put(reinterpret_cast<ITimerSpec*>(a3), old);
    return 0;
}

long sys_timerfd_gettime(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    FileRef ref;
    auto* t = lookup<TimerFd>(static_cast<int>(a0), KIND_TIMERFD, &ref.f);
    if (!t)
        return -err::INVAL;
    return uaccess::put(reinterpret_cast<ITimerSpec*>(a1), t->gettime());
}

long sys_signalfd4(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a2 != 8)
        return -err::INVAL;
    u64 mask;
    if (long r = uaccess::get(&mask, reinterpret_cast<u64*>(a1)); r < 0)
        return r;
    if (static_cast<int>(a0) != -1) { // update an existing signalfd (fd is a C int: compare 32 bits)
        FileRef ref;
        auto* s = lookup<SignalFd>(static_cast<int>(a0), KIND_SIGNALFD, &ref.f);
        if (!s)
            return -err::INVAL;
        s->mask = mask & ~UNBLOCKABLE;
        poll::notify();
        return static_cast<int>(a0);
    }
    if (a3 & ~static_cast<u64>(O_NONBLOCK_FLAG | O_CLOEXEC_FLAG))
        return -err::INVAL;
    auto* s = new SignalFd(mask);
    s->anon_kind = KIND_SIGNALFD;
    return install(s, static_cast<u32>(a3), "signalfd");
}

long sys_signalfd(SYSCALL_ARGS) { return sys_signalfd4(a0, a1, a2, 0, a4, a5, frame); }

long sys_epoll_create1(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a0 & ~static_cast<u64>(O_CLOEXEC_FLAG))
        return -err::INVAL;
    auto* e = new Epoll;
    e->anon_kind = KIND_EPOLL;
    return install(e, static_cast<u32>(a0), "eventpoll");
}

long sys_epoll_create(SYSCALL_ARGS) {
    if (static_cast<int>(a0) <= 0)
        return -err::INVAL;
    return sys_epoll_create1(0, a1, a2, a3, a4, a5, frame);
}

long sys_epoll_ctl(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    FileRef epref;
    auto* ep = lookup<Epoll>(static_cast<int>(a0), KIND_EPOLL, &epref.f);
    if (!ep)
        return proc::fd_get(proc::current(), static_cast<int>(a0)) ? -err::INVAL : -err::BADF;
    FileRef target;
    target.f = proc::fd_get(proc::current(), static_cast<int>(a2));
    if (!target.f)
        return -err::BADF;
    if (target.f->inode == ep)
        return -err::INVAL;
    int op = static_cast<int>(a1);
    EpollEvent ev{};
    if (op != EPOLL_CTL_DEL)
        if (long r = uaccess::get(&ev, reinterpret_cast<EpollEvent*>(a3)); r < 0)
            return r;
    if ((vfs::is_reg(target.f->inode->mode) || vfs::is_dir(target.f->inode->mode)) && !target.f->inode->epoll_regular())
        return -err::PERM; // like Linux: regular files and directories can't be watched (procfs can)
    return ep->ctl(op, static_cast<int>(a2), target.f, &ev);
}

long epoll_wait_common(long epfd, u64 user_events, int max, u64 timeout_ns) {
    if (max <= 0 || max > 65536)
        return -err::INVAL;
    FileRef ref;
    auto* ep = lookup<Epoll>(epfd, KIND_EPOLL, &ref.f);
    if (!ep)
        return -err::BADF;
    if (long r = uaccess::check(reinterpret_cast<void*>(user_events), static_cast<usize>(max) * sizeof(EpollEvent), true);
        r < 0)
        return r;
    int chunk = max < 256 ? max : 256;
    auto* buf = static_cast<EpollEvent*>(kmalloc(static_cast<usize>(chunk) * sizeof(EpollEvent)));
    if (!buf)
        return -err::NOMEM;
    long n = ep->wait(buf, chunk, timeout_ns);
    if (n > 0)
        if (long r = uaccess::copy_to(reinterpret_cast<void*>(user_events), buf, static_cast<usize>(n) * sizeof(EpollEvent));
            r < 0)
            n = r;
    kfree(buf);
    return n;
}

u64 ms_to_timeout(i64 ms) { return ms < 0 ? WAIT_FOREVER : static_cast<u64>(ms) * 1'000'000ull; }

long sys_epoll_wait(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return epoll_wait_common(static_cast<int>(a0), a1, static_cast<int>(a2), ms_to_timeout(static_cast<int>(a3)));
}

// epoll_pwait / epoll_pwait2: like epoll_wait with a temporary signal mask.
long pwait_with_mask(long epfd, u64 events, int max, u64 timeout_ns, u64 user_mask, u64 masksize) {
    sched::Thread* t = sched::current();
    u64 saved = t->sigmask;
    if (user_mask) {
        if (masksize != 8)
            return -err::INVAL;
        u64 mask;
        if (long r = uaccess::get(&mask, reinterpret_cast<u64*>(user_mask)); r < 0)
            return r;
        t->sigmask = mask & ~UNBLOCKABLE;
    }
    if (user_mask) { // the mask comes back on the way to user mode (after a handler, if one runs)
        t->saved_sigmask = saved;
        t->restore_sigmask = true;
    }
    return epoll_wait_common(epfd, events, max, timeout_ns);
}

long sys_epoll_pwait(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return pwait_with_mask(static_cast<int>(a0), a1, static_cast<int>(a2), ms_to_timeout(static_cast<int>(a3)), a4, a5);
}

long sys_epoll_pwait2(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    u64 timeout = WAIT_FOREVER;
    if (a3) {
        Timespec64 ts;
        if (long r = uaccess::get(&ts, reinterpret_cast<Timespec64*>(a3)); r < 0)
            return r;
        if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1'000'000'000)
            return -err::INVAL;
        timeout = to_ns(ts);
    }
    return pwait_with_mask(static_cast<int>(a0), a1, static_cast<int>(a2), timeout, a4, a5);
}

} // namespace

} // namespace events

namespace syscall {

void register_events() {
    vfs::file_release_hook = events::file_released;
    set(213, events::sys_epoll_create, "epoll_create");
    set(232, events::sys_epoll_wait, "epoll_wait");
    set(233, events::sys_epoll_ctl, "epoll_ctl");
    set(281, events::sys_epoll_pwait, "epoll_pwait");
    set(282, events::sys_signalfd, "signalfd");
    set(283, events::sys_timerfd_create, "timerfd_create");
    set(284, events::sys_eventfd, "eventfd");
    set(286, events::sys_timerfd_settime, "timerfd_settime");
    set(287, events::sys_timerfd_gettime, "timerfd_gettime");
    set(289, events::sys_signalfd4, "signalfd4");
    set(290, events::sys_eventfd2, "eventfd2");
    set(291, events::sys_epoll_create1, "epoll_create1");
    set(441, events::sys_epoll_pwait2, "epoll_pwait2");
}

} // namespace syscall
