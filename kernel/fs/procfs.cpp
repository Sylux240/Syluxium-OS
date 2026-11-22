#include <boot/limine_requests.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/percpu.hpp>
#include <fs/genfs.hpp>
#include <fs/procfs.hpp>
#include <fs/ramfs.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <mm/pmm.hpp>
#include <proc/process.hpp>
#include <sched/sched.hpp>
#include <time/clock.hpp>
#include <time/rtc.hpp>

namespace vfs { struct File; }
namespace events { long pidfd_file_pid(vfs::File* f); }

namespace procfs {

namespace {

using genfs::GeneratedFile;
using genfs::GeneratedLink;
using genfs::TextBuffer;
using proc::Process;

constexpr u64 USER_HZ = 100;               // clock ticks as user space sees them
constexpr u64 TICKS_PER_USER_HZ = sched::TICK_HZ / USER_HZ;

bool parse_number(const char* name, usize len, int* out) {
    if (len == 0 || len > 9)
        return false;
    int v = 0;
    for (usize i = 0; i < len; i++) {
        if (name[i] < '0' || name[i] > '9')
            return false;
        v = v * 10 + (name[i] - '0');
    }
    *out = v;
    return true;
}

bool name_is(const char* name, usize len, const char* want) {
    return strlen(want) == len && memcmp(name, want, len) == 0;
}

void fill_entry(vfs::DirEntry* out, u64 ino, u8 type, const char* name) {
    out->ino = ino;
    out->type = type;
    snprintf(out->name, sizeof(out->name), "%s", name);
}

// ---- snapshot of a process, taken under the table lock ----

struct Snapshot {
    int pid, ppid, pgid, sid, threads;
    char state;
    char name[64];
    char exe[256];
    char cwd[512];
    u64 utime, stime, start_ticks, vsize, rss;
    u64 sig_pending, sig_blocked;
    proc::Cred cred;
    proc::Memory* mem; // referenced (put() when done) or null
    int cpu;
};

// Find `pid` (and, if tid != 0, check that thread belongs to it).
bool snapshot(int pid, int tid, Snapshot* s, bool want_memory) {
    LockGuard guard(proc::table_lock());
    Process* p = proc::find(pid);
    if (!p)
        return false;
    s->pid = p->pid;
    s->ppid = p->parent ? p->parent->pid : (p->kernel_waited ? 0 : 1);
    s->pgid = p->pgid;
    s->sid = p->sid;
    // Inside a PID namespace: the numbers it uses.
    if (Process* viewer = proc::current(); viewer && viewer->pidns) {
        auto local = [&](int global) {
            Process* q = proc::find(global);
            return q && q->pidns == viewer->pidns ? q->vpid : 0;
        };
        s->pid = p->vpid;
        s->ppid = p->parent && p->parent->pidns == viewer->pidns ? p->parent->vpid : 0;
        s->pgid = local(p->pgid);
        s->sid = local(p->sid);
    }
    memcpy(s->name, p->name, sizeof(s->name));
    s->cred = p->cred;
    memcpy(s->exe, p->exe_path, sizeof(s->exe));
    memcpy(s->cwd, p->cwd_path, sizeof(s->cwd));
    s->start_ticks = p->start_ns / (1'000'000'000 / USER_HZ);
    s->sig_pending = p->sig_pending;
    s->sig_blocked = 0;
    s->cpu = 0;

    bool found_tid = tid == 0;
    bool running = false;
    u64 ut = p->dead_utime, st = p->dead_stime;
    {
        LockGuard tguard(p->thread_lock);
        s->threads = static_cast<int>(p->live_threads);
        for (sched::Thread* t : p->threads) {
            if (tid && t->user_tid != tid)
                continue;
            found_tid = true;
            if (tid) { ut = st = 0; }
            ut += t->utime_ticks;
            st += t->stime_ticks;
            if (t->state == sched::ThreadState::Running || t->state == sched::ThreadState::Ready)
                running = true;
            if (t->cpu)
                s->cpu = static_cast<int>(t->cpu->index);
            if (tid || t == p->main_thread)
                s->sig_blocked = t->sigmask;
        }
    }
    if (!found_tid)
        return false;
    s->utime = ut / TICKS_PER_USER_HZ;
    s->stime = st / TICKS_PER_USER_HZ;
    s->state = p->state == proc::State::Zombie ? 'Z' : (running ? 'R' : 'S');
    s->mem = nullptr;
    if (want_memory && p->mem) {
        s->mem = p->mem;
        s->mem->get();
    }
    s->vsize = s->rss = 0;
    if (p->mem)
        s->rss = p->mem->rss_pages();
    return true;
}

void release(Snapshot& s) {
    if (s.mem)
        s.mem->put();
}

const char* state_name(char c) {
    switch (c) {
    case 'R': return "R (running)";
    case 'Z': return "Z (zombie)";
    default:  return "S (sleeping)";
    }
}

// ---- per-process directory ----

class FdDir;
class TaskDir;

// /proc/<pid>/oom_score_adj (-1000..1000) and the older oom_adj (-17..15):
// readable and writable; TDE's start_tdeinit protects its core processes
// through them (and waits until it has).
class OomFile : public vfs::Inode {
public:
    OomFile(int p, bool legacy) : m_pid(p), m_legacy(legacy) {
        ino = vfs::next_ino();
        mode = vfs::S_IFREG | 0644;
        atime = mtime = ctime = vfs::now();
    }
    isize read(u64 offset, void* buf, usize count) override {
        int v;
        {
            LockGuard guard(proc::table_lock());
            Process* p = proc::find(m_pid);
            if (!p)
                return -err::SRCH;
            v = m_legacy ? p->oom_score_adj * 17 / 1000 : p->oom_score_adj;
        }
        char text[16];
        usize n = static_cast<usize>(snprintf(text, sizeof(text), "%d\n", v));
        if (offset >= n)
            return 0;
        n = min<usize>(n - offset, count);
        memcpy(buf, text + offset, n);
        return static_cast<isize>(n);
    }
    isize write(u64, const void* buf, usize count) override {
        char text[16] = {};
        memcpy(text, buf, min<usize>(count, sizeof(text) - 1));
        const char* q = text;
        bool neg = *q == '-';
        if (neg || *q == '+')
            q++;
        int v = 0;
        while (*q >= '0' && *q <= '9')
            v = v * 10 + (*q++ - '0');
        if (neg)
            v = -v;
        int lo = m_legacy ? -17 : -1000, hi = m_legacy ? 15 : 1000;
        if (v < lo || v > hi)
            return -err::INVAL;
        if (m_legacy)
            v = v == -17 ? -1000 : v * 1000 / 17;
        LockGuard guard(proc::table_lock());
        Process* p = proc::find(m_pid);
        if (!p)
            return -err::SRCH;
        if (v < p->oom_score_adj && !proc::capable())
            return -err::ACCES; // only root may protect a process more
        p->oom_score_adj = v;
        return static_cast<isize>(count);
    }
    long truncate(u64) override { return 0; }

private:
    int m_pid;
    bool m_legacy;
};

class PidDir : public vfs::Inode {
public:
    PidDir(int pid, int tid);
    vfs::Inode* lookup(const char* name, usize len) override;
    long readdir(u64 index, vfs::DirEntry* out) override;
    isize read(u64, void*, usize) override { return -err::ISDIR; }

    int pid, tid; // tid != 0: /proc/<pid>/task/<tid>
    PidDir* next_cached = nullptr;

private:
    struct Entry {
        const char* name;
        vfs::Inode* inode;
    };
    Entry m_entries[16];

public:
    void set_entry_owner(u32 u, u32 g) {
        for (int i = 0; i < m_count; i++) {
            m_entries[i].inode->uid = u;
            m_entries[i].inode->gid = g;
        }
    }

private:
    int m_count = 0;
    void add(const char* name, vfs::Inode* node) { m_entries[m_count++] = {name, node}; }
};

PidDir* g_pid_dirs = nullptr; // cache, never freed (one small object per pid ever looked at)
IrqSpinlock g_cache_lock;

// The /proc/<pid> directory belongs to the process's (effective) user, as on
// Linux: ps shows the owner as the process's user.
void set_owner_from(PidDir* d) {
    Snapshot s;
    if (snapshot(d->pid, d->tid, &s, false)) {
        d->uid = s.cred.euid;
        d->gid = s.cred.egid;
        d->set_entry_owner(s.cred.euid, s.cred.egid); // its files too (owner checks, as on Linux)
    }
}

PidDir* pid_dir(int pid, int tid) {
    {
        LockGuard guard(g_cache_lock);
        for (PidDir* d = g_pid_dirs; d; d = d->next_cached) {
            if (d->pid == pid && d->tid == tid) {
                set_owner_from(d); // pids are reused; ids change with setuid
                return d;
            }
        }
    }
    auto* d = new PidDir(pid, tid);
    set_owner_from(d);
    LockGuard guard(g_cache_lock);
    d->next_cached = g_pid_dirs;
    g_pid_dirs = d;
    return d;
}

bool exists(int pid, int tid) {
    Snapshot s;
    if (!snapshot(pid, tid, &s, false))
        return false;
    return true;
}

// stat: the single line ps/top parse.
bool gen_stat(TextBuffer& out, const void* ctx) {
    auto* d = static_cast<const PidDir*>(ctx);
    Snapshot s;
    if (!snapshot(d->pid, d->tid, &s, true))
        return false;
    if (s.mem)
        s.vsize = s.mem->virtual_size();
    out.printf("%d (%s) %c %d %d %d %d %d %u %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %d %ld %lu %lu %lu "
               "%lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %d %d %u %u %lu %lu %ld %lu %lu %lu "
               "%lu %lu %lu %lu %d\n",
               d->tid ? d->tid : s.pid, s.name, s.state, s.ppid, s.pgid, s.sid, 0x0501 /* tty: console */, -1,
               0u, 0ul, 0ul, 0ul, 0ul, s.utime, s.stime, 0l, 0l, 20l, 0l, s.threads, 0l, s.start_ticks, s.vsize,
               s.rss, ~0ul, 0ul, 0ul, 0ul, 0ul, 0ul, s.sig_pending, s.sig_blocked, 0ul, 0ul, 0ul, 0ul, 0ul, 17,
               s.cpu, 0u, 0u, 0ul, 0ul, 0l, 0ul, 0ul, 0ul, s.mem ? s.mem->arg_start : 0, s.mem ? s.mem->arg_end : 0,
               s.mem ? s.mem->env_start : 0, s.mem ? s.mem->env_end : 0, 0);
    release(s);
    return true;
}

bool gen_status(TextBuffer& out, const void* ctx) {
    auto* d = static_cast<const PidDir*>(ctx);
    Snapshot s;
    if (!snapshot(d->pid, d->tid, &s, true))
        return false;
    if (s.mem)
        s.vsize = s.mem->virtual_size();
    out.printf("Name:\t%s\nUmask:\t0022\nState:\t%s\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\nTracerPid:\t0\n"
               "Uid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nFDSize:\t%d\nVmSize:\t%8lu kB\nVmRSS:\t%8lu kB\n"
               "Threads:\t%d\nSigPnd:\t%016lx\nSigBlk:\t%016lx\nCpus_allowed_list:\t0-%u\n",
               s.name, state_name(s.state), s.pid, d->tid ? d->tid : s.pid, s.ppid, s.cred.ruid, s.cred.euid,
               s.cred.suid, s.cred.fsuid, s.cred.rgid, s.cred.egid, s.cred.sgid, s.cred.fsgid, proc::MAX_FDS,
               s.vsize / 1024, s.rss * 4, s.threads, s.sig_pending, s.sig_blocked, percpu::online_count() - 1);
    release(s);
    return true;
}

// /proc/<pid>/syscall: the system call the (main) thread is blocked in.
bool gen_syscall(TextBuffer& out, const void* ctx) {
    auto* d = static_cast<const PidDir*>(ctx);
    LockGuard guard(proc::table_lock());
    Process* p = proc::find(d->pid);
    if (!p)
        return false;
    sched::Thread* t = d->tid ? proc::find_thread(d->tid) : p->main_thread;
    if (!t)
        return false;
    if (t->cur_syscall < 0)
        out.append("running\n");
    else
        out.printf("%ld %#lx %#lx %#lx\n", t->cur_syscall, t->cur_args[0], t->cur_args[1], t->cur_args[2]);
    return true;
}

bool gen_statm(TextBuffer& out, const void* ctx) {
    auto* d = static_cast<const PidDir*>(ctx);
    Snapshot s;
    if (!snapshot(d->pid, d->tid, &s, true))
        return false;
    u64 size = s.mem ? s.mem->virtual_size() / 4096 : 0;
    out.printf("%lu %lu 0 0 0 0 0\n", size, s.rss);
    release(s);
    return true;
}

bool gen_comm(TextBuffer& out, const void* ctx) {
    auto* d = static_cast<const PidDir*>(ctx);
    Snapshot s;
    if (!snapshot(d->pid, d->tid, &s, false))
        return false;
    out.printf("%s\n", s.name);
    return true;
}

// cmdline / environ: NUL-separated strings straight from the process's memory.
bool gen_strings(TextBuffer& out, const void* ctx, bool environment) {
    auto* d = static_cast<const PidDir*>(ctx);
    Snapshot s;
    if (!snapshot(d->pid, d->tid, &s, true))
        return false;
    if (s.mem) {
        uptr start = environment ? s.mem->env_start : s.mem->arg_start;
        uptr end = environment ? s.mem->env_end : s.mem->arg_end;
        usize len = end > start ? min<usize>(end - start, 64 * KiB) : 0;
        if (len) {
            auto* buf = static_cast<char*>(kmalloc(len));
            if (buf) {
                usize got = s.mem->read_remote(start, buf, len);
                out.append(buf, got);
                kfree(buf);
            }
        }
    }
    release(s);
    return true;
}
bool gen_cmdline(TextBuffer& out, const void* ctx) { return gen_strings(out, ctx, false); }
bool gen_environ(TextBuffer& out, const void* ctx) { return gen_strings(out, ctx, true); }

bool gen_maps(TextBuffer& out, const void* ctx) {
    auto* d = static_cast<const PidDir*>(ctx);
    Snapshot s;
    if (!snapshot(d->pid, d->tid, &s, true))
        return false;
    if (s.mem) {
        s.mem->for_each_vma([](const proc::Vma& v, void* arg) {
            auto* o = static_cast<TextBuffer*>(arg);
            o->printf("%012lx-%012lx %c%c%cp %08lx 00:00 %lu", v.start, v.end,
                      v.prot & proc::PROT_READ ? 'r' : '-', v.prot & proc::PROT_WRITE ? 'w' : '-',
                      v.prot & proc::PROT_EXEC ? 'x' : '-', v.file ? v.file_offset : 0,
                      v.file ? v.file->ino : 0);
            if (v.kind == proc::VMA_STACK) o->append("                  [stack]");
            else if (v.kind == proc::VMA_HEAP) o->append("                  [heap]");
            o->append("\n");
        }, &out);
    }
    release(s);
    return true;
}

bool link_exe(char* out, usize size, const void* ctx) {
    auto* d = static_cast<const PidDir*>(ctx);
    Snapshot s;
    if (!snapshot(d->pid, d->tid, &s, false))
        return false;
    snprintf(out, size, "%s", s.exe);
    return true;
}

bool link_cwd(char* out, usize size, const void* ctx) {
    auto* d = static_cast<const PidDir*>(ctx);
    Snapshot s;
    if (!snapshot(d->pid, d->tid, &s, false))
        return false;
    snprintf(out, size, "%s", s.cwd);
    return true;
}

// /proc/<pid>/fd: one symlink per open descriptor.
class FdDir : public vfs::Inode {
public:
    explicit FdDir(int pid) : m_pid(pid) {
        ino = vfs::next_ino();
        mode = vfs::S_IFDIR | 0500;
        nlink = 2;
    }
    isize read(u64, void*, usize) override { return -err::ISDIR; }

    vfs::Inode* lookup(const char* name, usize len) override {
        int fd;
        if (!parse_number(name, len, &fd) || fd >= proc::MAX_FDS || !fd_open(fd))
            return nullptr;
        LockGuard guard(m_lock);
        if (!m_links[fd]) {
            auto* ctx = new LinkCtx{m_pid, fd};
            m_links[fd] = new GeneratedLink(fd_target, ctx);
        }
        return m_links[fd];
    }

    long readdir(u64 index, vfs::DirEntry* out) override {
        if (index < 2) {
            fill_entry(out, ino, vfs::DT_DIR, index == 0 ? "." : "..");
            return 1;
        }
        u64 seen = 2;
        for (int fd = 0; fd < proc::MAX_FDS; fd++) {
            if (!fd_open(fd))
                continue;
            if (seen++ == index) {
                char name[16];
                snprintf(name, sizeof(name), "%d", fd);
                fill_entry(out, ino + 1 + static_cast<u64>(fd), vfs::DT_LNK, name);
                return 1;
            }
        }
        return 0;
    }

private:
    struct LinkCtx {
        int pid, fd;
    };

    bool fd_open(int fd) {
        LockGuard guard(proc::table_lock());
        Process* p = proc::find(m_pid);
        return p && p->fds[fd];
    }

    static bool fd_target(char* out, usize size, const void* ctx) {
        auto* c = static_cast<const LinkCtx*>(ctx);
        LockGuard guard(proc::table_lock());
        Process* p = proc::find(c->pid);
        if (!p || !p->fds[c->fd])
            return false;
        vfs::File* f = p->fds[c->fd];
        snprintf(out, size, "%s", f->path ? f->path : "anon_inode:[unknown]");
        return true;
    }

    int m_pid;
    vfs::Inode* m_links[proc::MAX_FDS] = {};
    IrqSpinlock m_lock;
};

// /proc/<pid>/fdinfo: one text file per open descriptor, as Linux writes
// them (position, flags, mount, inode; "Pid:" for a pidfd, which polkit's
// pkexec checks against its caller).
class FdInfoDir : public vfs::Inode {
public:
    explicit FdInfoDir(int pid) : m_pid(pid) {
        ino = vfs::next_ino();
        mode = vfs::S_IFDIR | 0500;
        nlink = 2;
    }
    isize read(u64, void*, usize) override { return -err::ISDIR; }

    vfs::Inode* lookup(const char* name, usize len) override {
        int fd;
        if (!parse_number(name, len, &fd) || fd >= proc::MAX_FDS || !fd_open(fd))
            return nullptr;
        LockGuard guard(m_lock);
        if (!m_files[fd]) {
            auto* ctx = new Ctx{m_pid, fd};
            m_files[fd] = new GeneratedFile(gen_info, ctx, 0400);
        }
        // Owned by the process's user, like this directory (kept current by
        // /proc/<pid> on every lookup): a daemon that dropped root after
        // starting (dbus-daemon reads its own pidfds' "Pid:" here) must be
        // able to read them.
        m_files[fd]->uid = uid;
        m_files[fd]->gid = gid;
        return m_files[fd];
    }

    long readdir(u64 index, vfs::DirEntry* out) override {
        if (index < 2) {
            fill_entry(out, ino, vfs::DT_DIR, index == 0 ? "." : "..");
            return 1;
        }
        u64 seen = 2;
        for (int fd = 0; fd < proc::MAX_FDS; fd++) {
            if (!fd_open(fd))
                continue;
            if (seen++ == index) {
                char name[16];
                snprintf(name, sizeof(name), "%d", fd);
                fill_entry(out, ino + 1 + static_cast<u64>(fd), vfs::DT_REG, name);
                return 1;
            }
        }
        return 0;
    }

private:
    struct Ctx {
        int pid, fd;
    };

    bool fd_open(int fd) {
        LockGuard guard(proc::table_lock());
        Process* p = proc::find(m_pid);
        return p && p->fds[fd];
    }

    static bool gen_info(TextBuffer& out, const void* ctx) {
        auto* c = static_cast<const Ctx*>(ctx);
        u64 pos = 0, inode = 0;
        u32 flags = 0;
        long pidfd_pid = -1;
        {
            LockGuard guard(proc::table_lock());
            Process* p = proc::find(c->pid);
            if (!p || !p->fds[c->fd])
                return false;
            vfs::File* f = p->fds[c->fd];
            pos = f->offset;
            flags = f->flags;
            if (p->cloexec[c->fd])
                flags |= 02000000; // O_CLOEXEC
            inode = f->inode ? f->inode->ino : 0;
            pidfd_pid = events::pidfd_file_pid(f);
        }
        out.printf("pos:\t%llu\nflags:\t0%o\nmnt_id:\t0\nino:\t%llu\n", static_cast<unsigned long long>(pos),
                   flags, static_cast<unsigned long long>(inode));
        if (pidfd_pid > 0)
            out.printf("Pid:\t%ld\nNSpid:\t%ld\n", pidfd_pid, pidfd_pid);
        return true;
    }

    int m_pid;
    vfs::Inode* m_files[proc::MAX_FDS] = {};
    IrqSpinlock m_lock;
};

// /proc/<pid>/task: one directory per thread.
class TaskDir : public vfs::Inode {
public:
    explicit TaskDir(int pid) : m_pid(pid) {
        ino = vfs::next_ino();
        mode = vfs::S_IFDIR | 0555;
        nlink = 2;
    }
    isize read(u64, void*, usize) override { return -err::ISDIR; }

    // st_nlink = 2 + threads, as on Linux: Chromium's sandbox (Electron
    // apps) tells "single-threaded" by /proc/self/task having 3 links and
    // CHECK-fails on anything below.
    long revalidate() override {
        u32 threads = 0;
        LockGuard guard(proc::table_lock());
        if (Process* p = proc::find(m_pid)) {
            LockGuard tguard(p->thread_lock);
            for (sched::Thread* t : p->threads) {
                (void)t;
                threads++;
            }
        }
        nlink = 2 + threads;
        return 0;
    }

    vfs::Inode* lookup(const char* name, usize len) override {
        int tid;
        if (!parse_number(name, len, &tid))
            return nullptr;
        if (Process* viewer = proc::current(); viewer && viewer->pidns) {
            LockGuard guard(proc::table_lock());
            sched::Thread* t = proc::find_visible_thread(tid, viewer);
            if (!t || !t->process || t->process->pid != m_pid)
                return nullptr;
            tid = t->user_tid;
        }
        if (!exists(m_pid, tid))
            return nullptr;
        return pid_dir(m_pid, tid);
    }

    long readdir(u64 index, vfs::DirEntry* out) override {
        if (index < 2) {
            fill_entry(out, ino, vfs::DT_DIR, index == 0 ? "." : "..");
            return 1;
        }
        LockGuard guard(proc::table_lock());
        Process* p = proc::find(m_pid);
        if (!p)
            return 0;
        LockGuard tguard(p->thread_lock);
        u64 i = 2;
        for (sched::Thread* t : p->threads) {
            if (i++ == index) {
                char name[16];
                snprintf(name, sizeof(name), "%d", proc::tid_nr(t, proc::current()));
                fill_entry(out, ino + static_cast<u64>(t->user_tid), vfs::DT_DIR, name);
                return 1;
            }
        }
        return 0;
    }

private:
    int m_pid;
};

// /proc/<pid>/ns/<kind>: what setns() enters (containers: lxc-attach).
// Plain readable files here (Linux's are nsfs magic links).
constexpr u8 KIND_NSFS = 0xB5;
class NsFile : public vfs::Inode {
public:
    NsFile(int pid, const char* kind) : m_pid(pid) {
        ino = vfs::next_ino();
        mode = vfs::S_IFREG | 0444;
        anon_kind = KIND_NSFS;
        snprintf(m_kind, sizeof(m_kind), "%s", kind);
    }
    isize read(u64, void*, usize) override { return -err::INVAL; }
    int m_pid;
    char m_kind[16];
};

class NsDir : public vfs::Inode {
public:
    explicit NsDir(int pid) {
        ino = vfs::next_ino();
        mode = vfs::S_IFDIR | 0511;
        for (int i = 0; i < COUNT; i++)
            m_files[i] = new NsFile(pid, kinds[i]);
    }
    vfs::Inode* lookup(const char* name, usize len) override {
        for (int i = 0; i < COUNT; i++)
            if (name_is(name, len, kinds[i]))
                return m_files[i];
        return nullptr;
    }
    long readdir(u64 index, vfs::DirEntry* out) override {
        if (index >= COUNT)
            return 0;
        fill_entry(out, m_files[index]->ino, vfs::DT_REG, kinds[index]);
        return 1;
    }

private:
    static constexpr int COUNT = 7;
    static constexpr const char* kinds[COUNT] = {"mnt", "uts", "ipc", "net", "pid", "cgroup", "user"};
    NsFile* m_files[COUNT];
};

PidDir::PidDir(int p, int t) : pid(p), tid(t) {
    ino = vfs::next_ino();
    mode = vfs::S_IFDIR | 0555;
    nlink = 2;
    add("stat", new GeneratedFile(gen_stat, this));
    add("status", new GeneratedFile(gen_status, this));
    add("statm", new GeneratedFile(gen_statm, this));
    add("syscall", new GeneratedFile(gen_syscall, this, 0400));
    add("oom_score_adj", new OomFile(pid, false));
    add("oom_adj", new OomFile(pid, true));
    add("comm", new GeneratedFile(gen_comm, this, 0644));
    add("cmdline", new GeneratedFile(gen_cmdline, this));
    add("environ", new GeneratedFile(gen_environ, this, 0400));
    add("maps", new GeneratedFile(gen_maps, this));
    add("exe", new GeneratedLink(link_exe, this));
    add("cwd", new GeneratedLink(link_cwd, this));
    add("fd", new FdDir(pid));
    add("fdinfo", new FdInfoDir(pid));
    add("ns", new NsDir(pid));
    if (!tid)
        add("task", new TaskDir(pid));
}

vfs::Inode* PidDir::lookup(const char* name, usize len) {
    if (!exists(pid, tid))
        return nullptr;
    for (int i = 0; i < m_count; i++)
        if (name_is(name, len, m_entries[i].name))
            return m_entries[i].inode;
    return nullptr;
}

long PidDir::readdir(u64 index, vfs::DirEntry* out) {
    if (index < 2) {
        fill_entry(out, ino, vfs::DT_DIR, index == 0 ? "." : "..");
        return 1;
    }
    u64 i = index - 2;
    if (i >= static_cast<u64>(m_count))
        return 0;
    vfs::Inode* n = m_entries[i].inode;
    u8 type = vfs::is_dir(n->mode) ? vfs::DT_DIR : vfs::is_lnk(n->mode) ? vfs::DT_LNK : vfs::DT_REG;
    fill_entry(out, n->ino, type, m_entries[i].name);
    return 1;
}

// ---- /proc itself: static entries plus one directory per process ----

class ProcRoot : public ramfs::Dir {
public:
    long statfs(FsStats* out) override {
        *out = FsStats{0x9fa0 /* PROC_SUPER_MAGIC */, 4096, 0, 0, 0, 0};
        return 0;
    }
    vfs::Inode* lookup(const char* name, usize len) override {
        int pid;
        if (parse_number(name, len, &pid)) {
            // A number the caller's PID namespace uses: the process's own directory.
            Process* viewer = proc::current();
            if (viewer && viewer->pidns) {
                LockGuard guard(proc::table_lock());
                Process* p = proc::find_visible(pid, viewer);
                if (!p)
                    return nullptr;
                pid = p->pid;
            }
            return exists(pid, 0) ? pid_dir(pid, 0) : nullptr;
        }
        return Dir::lookup(name, len);
    }

    long readdir(u64 index, vfs::DirEntry* out) override {
        u64 statics = 2 + child_count();
        if (index < statics)
            return Dir::readdir(index, out);
        struct Search {
            u64 want, seen;
            int found;
            Process* viewer;
        } search{index - statics, 0, -1, proc::current()};
        proc::for_each([](Process* p, void* arg) {
            auto* st = static_cast<Search*>(arg);
            if (!proc::visible(p, st->viewer))
                return;
            if (st->seen++ == st->want)
                st->found = st->viewer && st->viewer->pidns ? p->vpid : p->pid;
        }, &search);
        if (search.found < 0)
            return 0;
        char name[16];
        snprintf(name, sizeof(name), "%d", search.found);
        fill_entry(out, 0x10000000ull + static_cast<u64>(search.found), vfs::DT_DIR, name);
        return 1;
    }
};

// ---- system-wide files ----

bool gen_meminfo(TextBuffer& out, const void*) {
    auto st = pmm::stats();
    u64 total = st.total_pages * 4, free = st.free_pages * 4;
    out.printf("MemTotal:       %8lu kB\nMemFree:        %8lu kB\nMemAvailable:   %8lu kB\n"
               "Buffers:               0 kB\nCached:                0 kB\nSwapCached:            0 kB\n"
               "Active:         %8lu kB\nInactive:              0 kB\nSwapTotal:             0 kB\n"
               "SwapFree:              0 kB\nShmem:                 0 kB\nSlab:                  0 kB\n",
               total, free, free, total - free);
    return true;
}

bool gen_uptime(TextBuffer& out, const void*) {
    u64 up = clock::now_ns() / 10'000'000; // hundredths of a second
    u64 idle = 0;
    for (u32 i = 0; i < percpu::count(); i++)
        if (Cpu* c = percpu::get(i))
            idle += c->idle_ticks;
    idle /= TICKS_PER_USER_HZ;
    out.printf("%lu.%02lu %lu.%02lu\n", up / 100, up % 100, idle / 100, idle % 100);
    return true;
}

bool gen_stat_global(TextBuffer& out, const void*) {
    u64 user = 0, sys = 0, idle = 0, ctxt = 0;
    u32 n = percpu::count();
    for (u32 i = 0; i < n; i++) {
        if (Cpu* c = percpu::get(i)) {
            user += c->user_ticks;
            sys += c->system_ticks;
            idle += c->idle_ticks;
            ctxt += c->context_switches;
        }
    }
    out.printf("cpu  %lu 0 %lu %lu 0 0 0 0 0 0\n", user / TICKS_PER_USER_HZ, sys / TICKS_PER_USER_HZ,
               idle / TICKS_PER_USER_HZ);
    for (u32 i = 0; i < n; i++)
        if (Cpu* c = percpu::get(i))
            out.printf("cpu%u %lu 0 %lu %lu 0 0 0 0 0 0\n", c->index, c->user_ticks / TICKS_PER_USER_HZ,
                       c->system_ticks / TICKS_PER_USER_HZ, c->idle_ticks / TICKS_PER_USER_HZ);
    out.printf("intr 0\nctxt %lu\nbtime %lu\nprocesses %u\nprocs_running 1\nprocs_blocked 0\n", ctxt,
               rtc::boot_epoch_seconds(), proc::count());
    return true;
}

bool gen_loadavg(TextBuffer& out, const void*) {
    u64 l[3];
    sched::load_average(l);
    u32 running = 0;
    for (u32 i = 0; i < percpu::count(); i++)
        if (Cpu* c = percpu::get(i); c && c->online)
            running += static_cast<u32>(c->rq.ready.size()) + (c->current && c->current != c->rq.idle ? 1 : 0);
    out.printf("%lu.%02lu %lu.%02lu %lu.%02lu %u/%lu %u\n", l[0] >> 11, ((l[0] & 2047) * 100) >> 11,
               l[1] >> 11, ((l[1] & 2047) * 100) >> 11, l[2] >> 11, ((l[2] & 2047) * 100) >> 11,
               running ? running : 1, sched::thread_count(), proc::count());
    return true;
}

void cpu_brand(char* brand) {
    memset(brand, 0, 49);
    if (arch::cpuid(0x80000000).eax < 0x80000004)
        return;
    for (u32 i = 0; i < 3; i++) {
        auto r = arch::cpuid(0x80000002 + i);
        memcpy(brand + i * 16 + 0, &r.eax, 4);
        memcpy(brand + i * 16 + 4, &r.ebx, 4);
        memcpy(brand + i * 16 + 8, &r.ecx, 4);
        memcpy(brand + i * 16 + 12, &r.edx, 4);
    }
}

bool gen_cpuinfo(TextBuffer& out, const void*) {
    auto l0 = arch::cpuid(0);
    char vendor[13];
    memcpy(vendor, &l0.ebx, 4);
    memcpy(vendor + 4, &l0.edx, 4);
    memcpy(vendor + 8, &l0.ecx, 4);
    vendor[12] = 0;
    char brand[49];
    cpu_brand(brand);
    const char* model_name = brand;
    while (*model_name == ' ') model_name++;

    auto l1 = arch::cpuid(1);
    u32 family = (l1.eax >> 8) & 0xF, model = (l1.eax >> 4) & 0xF, stepping = l1.eax & 0xF;
    if (family == 0xF) family += (l1.eax >> 20) & 0xFF;
    if (family >= 6) model |= ((l1.eax >> 16) & 0xF) << 4;
    auto l7 = arch::cpuid(7, 0);
    auto le = arch::cpuid(0x80000001);

    // A readable subset of the feature flags, Linux spelling.
    struct Flag { u32 reg, bit; const char* name; };
    static const Flag flags[] = {
        {0, 0, "fpu"}, {0, 4, "tsc"}, {0, 5, "msr"}, {0, 6, "pae"}, {0, 8, "cx8"}, {0, 9, "apic"},
        {0, 15, "cmov"}, {0, 16, "pat"}, {0, 19, "clflush"}, {0, 23, "mmx"}, {0, 24, "fxsr"},
        {0, 25, "sse"}, {0, 26, "sse2"}, {0, 28, "ht"}, {1, 0, "pni"}, {1, 1, "pclmulqdq"},
        {1, 9, "ssse3"}, {1, 12, "fma"}, {1, 13, "cx16"}, {1, 19, "sse4_1"}, {1, 20, "sse4_2"},
        {1, 21, "x2apic"}, {1, 22, "movbe"}, {1, 23, "popcnt"}, {1, 25, "aes"}, {1, 26, "xsave"},
        {1, 28, "avx"}, {1, 29, "f16c"}, {1, 30, "rdrand"}, {2, 11, "syscall"}, {2, 20, "nx"},
        {2, 26, "pdpe1gb"}, {2, 27, "rdtscp"}, {2, 29, "lm"}, {3, 3, "bmi1"}, {3, 5, "avx2"},
        {3, 8, "bmi2"}, {3, 9, "erms"}, {3, 16, "avx512f"}, {3, 18, "rdseed"}, {3, 19, "adx"},
        {3, 29, "sha_ni"},
    };
    u32 regs[4] = {l1.edx, l1.ecx, le.edx, l7.ebx};

    u32 n = percpu::count();
    for (u32 i = 0; i < n; i++) {
        Cpu* c = percpu::get(i);
        if (!c)
            continue;
        out.printf("processor\t: %u\nvendor_id\t: %s\ncpu family\t: %u\nmodel\t\t: %u\nmodel name\t: %s\n"
                   "stepping\t: %u\ncpu MHz\t\t: %lu.000\nphysical id\t: 0\nsiblings\t: %u\ncore id\t\t: %u\n"
                   "cpu cores\t: %u\napicid\t\t: %u\nfpu\t\t: yes\nflags\t\t:",
                   c->index, vendor, family, model, model_name, stepping, clock::tsc_frequency_hz() / 1'000'000,
                   n, c->index, n, c->lapic_id);
        for (const Flag& f : flags)
            if (regs[f.reg] & (1u << f.bit))
                out.printf(" %s", f.name);
        out.printf("\nbogomips\t: %lu.00\naddress sizes\t: 48 bits physical, 48 bits virtual\n\n",
                   clock::tsc_frequency_hz() / 500'000);
    }
    return true;
}

bool gen_mounts(TextBuffer& out, const void*) {
    vfs::for_each_mount([](const char* source, const char* path, const char* type, const char* options, void* arg) {
        auto* o = static_cast<TextBuffer*>(arg);
        o->printf("%s %s %s %s 0 0\n", source, path, type, options[0] ? options : "rw,relatime");
    }, &out);
    if (vfs::root() == vfs::ram_root()) // on a disk root they're real mount entries
        out.append("proc /proc proc rw,nosuid,nodev,noexec 0 0\nsysfs /sys sysfs rw,nosuid,nodev,noexec 0 0\n"
               "devtmpfs /dev devtmpfs rw 0 0\n");
    return true;
}

bool link_self(char* out, usize size, const void*) {
    Process* p = proc::current();
    if (!p)
        return false;
    snprintf(out, size, "%d", p->vpid);
    return true;
}

bool link_thread_self(char* out, usize size, const void*) {
    Process* p = proc::current();
    if (!p)
        return false;
    snprintf(out, size, "%d/task/%d", p->vpid, sched::current()->vtid);
    return true;
}

} // namespace

void init() {
    // Replace the (empty) /proc directory from the root filesystem.
    vfs::Inode* root = vfs::root();
    root->unlink("proc", 4, true);
    auto* procroot = new ProcRoot;
    procroot->mode = vfs::S_IFDIR | 0555;
    procroot->dev = 4; // its own st_dev, so tools see /proc as a separate mount
    static_cast<ramfs::Dir*>(root)->attach("proc", 4, procroot);

    auto add = [&](const char* name, vfs::Inode* node) { procroot->attach(name, strlen(name), node); };
    add("meminfo", new GeneratedFile(gen_meminfo, nullptr));
    add("uptime", new GeneratedFile(gen_uptime, nullptr));
    add("stat", new GeneratedFile(gen_stat_global, nullptr));
    add("loadavg", new GeneratedFile(gen_loadavg, nullptr));
    add("cpuinfo", new GeneratedFile(gen_cpuinfo, nullptr));
    add("self", new GeneratedLink(link_self, nullptr));
    add("thread-self", new GeneratedLink(link_thread_self, nullptr));

    genfs::static_file("/proc/version",
                       "Linux version 6.12.0-syluxium (syluxium@build) (clang) #1 SMP Syluxium " __DATE__ "\n");
    {
        static char cmdline[260];
        snprintf(cmdline, sizeof(cmdline), "%s\n", boot::cmdline());
        genfs::static_file("/proc/cmdline", cmdline);
    }
    genfs::static_file("/proc/filesystems", "nodev\tramfs\nnodev\tproc\nnodev\tsysfs\nnodev\tdevtmpfs\nnodev\ttmpfs\nnodev\tfuse\n\tvfat\n\texfat\n\text4\n\text3\n\text2\n");
    add("mounts", new GeneratedFile(gen_mounts, nullptr));
    vfs::mkdir_p("/proc/sys/kernel");
    genfs::static_file("/proc/sys/kernel/hostname", "syluxium\n");
    genfs::static_file("/proc/sys/kernel/ostype", "Linux\n");
    genfs::static_file("/proc/sys/kernel/osrelease", "6.12.0-syluxium\n");
    genfs::static_file("/proc/sys/kernel/pid_max", "4194304\n");
    vfs::mkdir_p("/etc");
    // Many tools read /etc/mtab; on Linux it's a link to /proc/self/mounts.
    vfs::Inode* etc;
    if (vfs::resolve("/etc", nullptr, true, &etc) == 0 && !etc->lookup("mtab", 4))
        etc->create("mtab", 4, vfs::S_IFLNK | 0777, "/proc/mounts", nullptr);
}

} // namespace procfs

// ---------------------------------------------------------------------------

namespace sysfs {

namespace {
vfs::Inode* g_sysfs_root = nullptr;
} // namespace

bool contains(const void* inode) {
    // Directories know their parent (".."); files don't (sd-device checks directories).
    auto* n = static_cast<const vfs::Inode*>(inode);
    for (int depth = 0; n && g_sysfs_root && depth < 64; depth++) {
        if (n == g_sysfs_root)
            return true;
        if (n->parent == n)
            break;
        n = n->parent;
    }
    return false;
}

void init() {
    vfs::Inode* root = vfs::root();
    root->unlink("sys", 3, true);
    vfs::mkdir_p("/sys/devices/system/cpu", 0755);
    vfs::mkdir_p("/sys/kernel", 0755);
    vfs::mkdir_p("/sys/class", 0755);
    vfs::mkdir_p("/sys/bus", 0755);
    vfs::mkdir_p("/sys/block", 0755);
    vfs::mkdir_p("/sys/firmware", 0755);

    u32 cpus = percpu::online_count();
    char range[32];
    snprintf(range, sizeof(range), cpus > 1 ? "0-%u\n" : "0\n", cpus - 1);
    genfs::static_file("/sys/devices/system/cpu/online", range);
    genfs::static_file("/sys/devices/system/cpu/possible", range);
    genfs::static_file("/sys/devices/system/cpu/present", range);
    char path[96];
    for (u32 i = 0; i < cpus; i++) {
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/online", i);
        vfs::mkdir_p("/sys/devices/system/cpu", 0755);
        char dir[96];
        snprintf(dir, sizeof(dir), "/sys/devices/system/cpu/cpu%u", i);
        vfs::mkdir_p(dir, 0755);
        genfs::static_file(path, "1\n");
    }
    genfs::static_file("/sys/kernel/hostname", "syluxium\n");
    // Separate st_dev values so tools (df, findmnt) tell these mounts apart from /.
    vfs::Inode* n;
    if (vfs::resolve("/sys", nullptr, true, &n) == 0) {
        n->dev = 5;
        g_sysfs_root = n;
    }
    if (vfs::resolve("/dev", nullptr, true, &n) == 0)
        n->dev = 6;
}

} // namespace sysfs

namespace proc {
// setns(): which process and kind a /proc/<pid>/ns/<kind> file names.
long ns_file_target(vfs::File* f, int* pid, char* kind, usize size) {
    if (f->inode->anon_kind != procfs::KIND_NSFS)
        return -err::INVAL;
    auto* n = static_cast<procfs::NsFile*>(f->inode);
    *pid = n->m_pid;
    snprintf(kind, size, "%s", n->m_kind);
    return 0;
}
} // namespace proc
