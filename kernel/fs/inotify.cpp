#include <fs/inotify.hpp>
#include <fs/poll.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <proc/process.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>
#include <sched/sync.hpp>
#include <syscall/syscall.hpp>

namespace inotify {

namespace {

constexpr u32 IN_Q_OVERFLOW = 0x4000, IN_IGNORED = 0x8000, IN_MASK_CREATE = 0x10000000, IN_MASK_ADD = 0x20000000,
              IN_ONESHOT = 0x80000000;
constexpr u32 IN_ALL_EVENTS = 0xFFF;
constexpr usize MAX_QUEUED = 16384;

struct Event {
    i32 wd;
    u32 mask, cookie;
    u32 len;     // bytes of name, NUL-padded to a multiple of 16 (0 = no name)
    char name[]; // follows in the queue
};

struct Queued {
    Queued* next;
    usize bytes; // sizeof(header) + len
    Event ev;
};

class Instance;

struct Watch {
    Instance* owner;
    int wd;
    vfs::Inode* inode; // compared, never dereferenced
    u32 mask;
    Watch* next;
};

// One lock for all watches and queues: events are small and rare.
IrqSpinlock g_lock;
Watch* g_watches = nullptr;
volatile u32 g_watch_count = 0;
u32 g_cookie = 1;

class Instance : public vfs::Inode {
public:
    Instance() {
        ino = vfs::next_ino();
        mode = 0600;
        atime = mtime = ctime = vfs::now();
        anon_kind = 0xA1; // inotify (checked by the syscalls)
    }

    bool seekable() const override { return false; }
    u64 poll_generation() const override { return __atomic_load_n(&m_generation, __ATOMIC_ACQUIRE) + 1; }

    // Queue an event (g_lock held). Identical back-to-back events merge.
    void push(int wd, u32 mask, u32 cookie, const char* name, usize name_len) {
        usize len = name_len ? align_up<usize>(name_len + 1, 16) : 0;
        if (m_tail && m_tail->ev.wd == wd && m_tail->ev.mask == mask && m_tail->ev.cookie == cookie &&
            m_tail->ev.len == len && (!len || (memcmp(m_tail->ev.name, name, name_len) == 0 && !m_tail->ev.name[name_len])))
            return;
        if (m_count >= MAX_QUEUED) {
            if (m_overflowed)
                return;
            m_overflowed = true;
            wd = -1;
            mask = IN_Q_OVERFLOW;
            cookie = 0;
            len = name_len = 0;
        }
        auto* q = static_cast<Queued*>(kzalloc(sizeof(Queued) + len));
        if (!q)
            return;
        q->bytes = sizeof(Event) + len;
        q->ev = Event{wd, mask, cookie, static_cast<u32>(len)};
        if (name_len)
            memcpy(q->ev.name, name, name_len);
        if (m_tail)
            m_tail->next = q;
        else
            m_head = q;
        m_tail = q;
        m_count++;
        m_bytes += q->bytes;
        m_changed = true;
        __atomic_add_fetch(&m_generation, 1, __ATOMIC_RELEASE);
    }

    isize read_file(vfs::File* f, u64, void* buf, usize count) override {
        // Events are gathered into a kernel buffer under the spinlock and
        // copied to the user afterwards (that copy may fault and sleep).
        usize cap = count < 65536 ? count : 65536;
        u8* kbuf = static_cast<u8*>(kmalloc(cap ? cap : 1));
        if (!kbuf)
            return -err::NOMEM;
        for (;;) {
            usize done = 0;
            {
                LockGuard guard(g_lock);
                if (m_head && m_head->bytes > cap) {
                    kfree(kbuf);
                    return -err::INVAL; // buffer too small for the next event
                }
                while (m_head && done + m_head->bytes <= cap) {
                    Queued* q = m_head;
                    memcpy(kbuf + done, &q->ev, q->bytes);
                    done += q->bytes;
                    m_bytes -= q->bytes;
                    m_head = q->next;
                    if (!m_head)
                        m_tail = nullptr;
                    m_count--;
                    kfree(q);
                }
                if (!m_count)
                    m_overflowed = false;
            }
            if (done) {
                memcpy(buf, kbuf, done);
                kfree(kbuf);
                return static_cast<isize>(done);
            }
            if (f->flags & vfs::O_NONBLOCK || signal::interrupted()) {
                kfree(kbuf);
                return (f->flags & vfs::O_NONBLOCK) ? -err::AGAIN : -err::INTR;
            }
            m_wq.wait_until([&] { return m_head != nullptr || signal::interrupted(); });
        }
    }

    u16 poll(vfs::File*) override { return m_head ? poll::POLLIN : 0; }

    long ioctl(u64 request, u64 arg) override {
        if (request != 0x541B) // FIONREAD
            return -err::NOTTY;
        i32 n = static_cast<i32>(m_bytes);
        return uaccess::put(reinterpret_cast<i32*>(arg), n);
    }

    long add_watch(vfs::Inode* target, u32 mask) {
        LockGuard guard(g_lock);
        for (Watch* w = g_watches; w; w = w->next) {
            if (w->owner != this || w->inode != target)
                continue;
            if (mask & IN_MASK_CREATE)
                return -err::EXIST;
            w->mask = (mask & IN_MASK_ADD) ? (w->mask | mask) : mask;
            return w->wd;
        }
        auto* w = new Watch{this, m_next_wd++, target, mask, g_watches};
        g_watches = w;
        __atomic_add_fetch(&g_watch_count, 1, __ATOMIC_RELAXED);
        return w->wd;
    }

    long rm_watch(int wd) {
        {
            LockGuard guard(g_lock);
            Watch** link = &g_watches;
            while (*link && !((*link)->owner == this && (*link)->wd == wd))
                link = &(*link)->next;
            if (!*link)
                return -err::INVAL;
            Watch* w = *link;
            *link = w->next;
            delete w;
            __atomic_sub_fetch(&g_watch_count, 1, __ATOMIC_RELAXED);
            push(wd, IN_IGNORED, 0, nullptr, 0);
        }
        wake();
        return 0;
    }

    void wake() {
        if (m_changed) {
            m_changed = false;
            m_wq.wake_all();
            poll::notify();
        }
    }

    // Only the last of the Files sharing this inode frees it (see events.cpp).
    void on_open(vfs::File*) override { __atomic_add_fetch(&m_opens, 1, __ATOMIC_ACQ_REL); }

    void on_release(vfs::File*) override {
        if (__atomic_sub_fetch(&m_opens, 1, __ATOMIC_ACQ_REL) != 0)
            return;
        {
            LockGuard guard(g_lock);
            Watch** link = &g_watches;
            while (*link) {
                if ((*link)->owner == this) {
                    Watch* w = *link;
                    *link = w->next;
                    delete w;
                    __atomic_sub_fetch(&g_watch_count, 1, __ATOMIC_RELAXED);
                } else {
                    link = &(*link)->next;
                }
            }
            while (Queued* q = m_head) {
                m_head = q->next;
                kfree(q);
            }
        }
        delete this;
    }

    bool m_changed = false;
    u64 m_generation = 0; // events queued so far (edge-triggered epoll)

private:
    u32 m_opens = 0;
    Queued* m_head = nullptr;
    Queued* m_tail = nullptr;
    usize m_count = 0, m_bytes = 0;
    bool m_overflowed = false;
    int m_next_wd = 1;
    sched::WaitQueue m_wq;
};

// Deliver to every watch on `inode` whose mask wants `mask` (g_lock held).
// Collects instances to wake afterwards.
void deliver(vfs::Inode* inode, u32 mask, u32 cookie, const char* name, usize len, Instance** woken, int* nwoken) {
    for (Watch** link = &g_watches; *link;) {
        Watch* w = *link;
        if (w->inode != inode || !(w->mask & mask & IN_ALL_EVENTS)) {
            link = &w->next;
            continue;
        }
        w->owner->push(w->wd, mask, cookie, name, len);
        if (*nwoken < 32)
            woken[(*nwoken)++] = w->owner;
        if (w->mask & IN_ONESHOT) {
            w->owner->push(w->wd, IN_IGNORED, 0, nullptr, 0);
            *link = w->next;
            delete w;
            __atomic_sub_fetch(&g_watch_count, 1, __ATOMIC_RELAXED);
            continue;
        }
        link = &w->next;
    }
}

void wake_all(Instance** woken, int n) {
    for (int i = 0; i < n; i++)
        woken[i]->wake();
}

} // namespace

bool active() { return __atomic_load_n(&g_watch_count, __ATOMIC_RELAXED) != 0; }

u32 next_cookie() {
    LockGuard guard(g_lock);
    return g_cookie++;
}

void child_event(vfs::Inode* dir, u32 mask, const char* name, usize len, u32 cookie) {
    if (!active() || !dir)
        return;
    Instance* woken[32];
    int n = 0;
    {
        LockGuard guard(g_lock);
        deliver(dir, mask, cookie, name, len, woken, &n);
    }
    wake_all(woken, n);
}

void self_event(vfs::Inode* inode, u32 mask) {
    if (!active() || !inode)
        return;
    Instance* woken[32];
    int n = 0;
    {
        LockGuard guard(g_lock);
        deliver(inode, mask, 0, nullptr, 0, woken, &n);
    }
    wake_all(woken, n);
}

void file_event(vfs::File* f, u32 mask) {
    if (!active() || !f || !f->inode)
        return;
    if (vfs::is_dir(f->inode->mode))
        mask |= IN_ISDIR;
    self_event(f->inode, mask);
    if (!f->path || f->path[0] != '/')
        return;
    vfs::Inode* parent;
    const char* name;
    usize len;
    if (vfs::resolve_parent(f->path, nullptr, &parent, &name, &len) == 0)
        child_event(parent, mask, name, len);
}

void inode_gone(vfs::Inode* inode) {
    if (!active())
        return;
    Instance* woken[32];
    int n = 0;
    {
        LockGuard guard(g_lock);
        for (Watch** link = &g_watches; *link;) {
            Watch* w = *link;
            if (w->inode != inode) {
                link = &w->next;
                continue;
            }
            if (w->mask & IN_DELETE_SELF)
                w->owner->push(w->wd, IN_DELETE_SELF, 0, nullptr, 0);
            w->owner->push(w->wd, IN_IGNORED, 0, nullptr, 0);
            if (n < 32)
                woken[n++] = w->owner;
            *link = w->next;
            delete w;
            __atomic_sub_fetch(&g_watch_count, 1, __ATOMIC_RELAXED);
        }
    }
    wake_all(woken, n);
}

} // namespace inotify

// ---- system calls ----

namespace syscall {

namespace {

using inotify::Instance;

long sys_inotify_init1(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    constexpr u32 IN_NONBLOCK = 04000, IN_CLOEXEC = 02000000;
    u32 flags = static_cast<u32>(a0);
    if (flags & ~(IN_NONBLOCK | IN_CLOEXEC))
        return -err::INVAL;
    auto* in = new Instance;
    vfs::File* f = vfs::open_inode(in, vfs::O_RDONLY | ((flags & IN_NONBLOCK) ? vfs::O_NONBLOCK : 0));
    vfs::file_set_path(f, "anon_inode:inotify");
    long fd = proc::fd_install(proc::current(), f, 0, flags & IN_CLOEXEC);
    if (fd < 0)
        vfs::file_put(f);
    return fd;
}

long sys_inotify_init(SYSCALL_ARGS) { return sys_inotify_init1(0, a1, a2, a3, a4, a5, frame); }

Instance* instance_of(long fd, vfs::File** out) {
    vfs::File* f = proc::fd_get(proc::current(), static_cast<int>(fd));
    if (!f)
        return nullptr;
    if (f->inode->anon_kind != 0xA1) {
        vfs::file_put(f);
        return nullptr;
    }
    *out = f;
    return static_cast<Instance*>(f->inode);
}

long sys_inotify_add_watch(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    constexpr u32 IN_ONLYDIR = 0x01000000, IN_DONT_FOLLOW = 0x02000000;
    u32 mask = static_cast<u32>(a2);
    if (!(mask & 0xFFF))
        return -err::INVAL;
    vfs::File* f;
    Instance* in = instance_of(static_cast<int>(a0), &f);
    if (!in)
        return proc::fd_get(proc::current(), static_cast<int>(a0)) ? -err::INVAL : -err::BADF;
    char* path = static_cast<char*>(kmalloc(4096));
    long r = path ? uaccess::string_from(reinterpret_cast<const char*>(a1), path, 4096) : -err::NOMEM;
    vfs::Inode* target = nullptr;
    if (r >= 0)
        r = vfs::resolve(path, proc::current()->cwd, !(mask & IN_DONT_FOLLOW), &target);
    if (r >= 0 && (mask & IN_ONLYDIR) && !vfs::is_dir(target->mode))
        r = -err::NOTDIR;
    if (r >= 0)
        r = in->add_watch(target, mask);
    kfree(path);
    vfs::file_put(f);
    return r;
}

long sys_inotify_rm_watch(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    vfs::File* f;
    Instance* in = instance_of(static_cast<int>(a0), &f);
    if (!in)
        return -err::INVAL;
    long r = in->rm_watch(static_cast<int>(a1));
    vfs::file_put(f);
    return r;
}

} // namespace

void register_inotify() {
    set(253, sys_inotify_init, "inotify_init");
    set(254, sys_inotify_add_watch, "inotify_add_watch");
    set(255, sys_inotify_rm_watch, "inotify_rm_watch");
    set(294, sys_inotify_init1, "inotify_init1");
}

} // namespace syscall
