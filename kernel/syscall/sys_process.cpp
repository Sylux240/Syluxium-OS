#include <arch/x86_64/protect.hpp>
#include <arch/x86_64/compat_segments.hpp>
#include <core/bootlog.hpp>
#include <fs/ext2.hpp>
#include <block/block.hpp>
#include <acpi/power.hpp>
// Process, identity, signal-bookkeeping and system-information calls.

#include <core/log.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/percpu.hpp>
#include <lib/format.hpp>
#include <lib/random.hpp>
#include <lib/string.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <mm/heap.hpp>
#include <proc/futex.hpp>
#include <proc/process.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>
#include <sched/sched.hpp>
#include <syscall/syscall.hpp>
#include <time/clock.hpp>
#include <time/rtc.hpp>

namespace syscall {

namespace {

constexpr u32 MSR_FS_BASE = 0xC0000100;
constexpr u32 MSR_KERNEL_GS_BASE = 0xC0000102;

using proc::Process;

long sys_exit(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    proc::exit_thread(static_cast<int>(a0 & 0xFF)); // just this thread (the last one ends the process)
}

long sys_exit_group(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    proc::exit_current(static_cast<int>(a0 & 0xFF) << 8);
}

// Process ids are as the caller's PID namespace numbers them.
long sys_getpid(SYSCALL_ARGS) { SYSCALL_UNUSED; return proc::current()->vpid; }
long sys_getppid(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Process* me = proc::current();
    Process* parent = me->parent;
    if (!parent)
        return me->pidns ? 0 : 1; // orphans belong to "init", as on Linux
    return proc::pid_nr(parent, me); // 0 for a namespace's init (its parent is outside)
}
long sys_gettid(SYSCALL_ARGS) { SYSCALL_UNUSED; return sched::current()->vtid; }
// ---- user and group ids (POSIX/Linux rules; privileged = effective uid 0) ----

namespace {

proc::Cred& my_cred() { return proc::current()->cred; }
constexpr u32 KEEP = 0xFFFFFFFF; // -1: leave this id alone

bool one_of(u32 v, u32 a, u32 b, u32 c) { return v == a || v == b || v == c; }

// setresuid/setresgid core: unprivileged callers may only shuffle their own ids.
long set_res(u32* real, u32* eff, u32* saved, u32* fs, u32 r, u32 e, u32 sv, bool privileged) {
    if (!privileged) {
        if ((r != KEEP && !one_of(r, *real, *eff, *saved)) || (e != KEEP && !one_of(e, *real, *eff, *saved)) ||
            (sv != KEEP && !one_of(sv, *real, *eff, *saved)))
            return -err::PERM;
    }
    if (r != KEEP) *real = r;
    if (e != KEEP) *eff = e;
    if (sv != KEEP) *saved = sv;
    *fs = *eff;
    return 0;
}

} // namespace

long sys_getuid(SYSCALL_ARGS) { SYSCALL_UNUSED; return my_cred().ruid; }
long sys_geteuid(SYSCALL_ARGS) { SYSCALL_UNUSED; return my_cred().euid; }
long sys_getgid(SYSCALL_ARGS) { SYSCALL_UNUSED; return my_cred().rgid; }
long sys_getegid(SYSCALL_ARGS) { SYSCALL_UNUSED; return my_cred().egid; }

long sys_setuid(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    proc::Cred& c = my_cred();
    u32 uid = static_cast<u32>(a0);
    if (c.euid == 0) {
        c.ruid = c.euid = c.suid = c.fsuid = uid;
        return 0;
    }
    if (uid != c.ruid && uid != c.suid)
        return -err::PERM;
    c.euid = c.fsuid = uid;
    return 0;
}

long sys_setgid(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    proc::Cred& c = my_cred();
    u32 gid = static_cast<u32>(a0);
    if (c.euid == 0) {
        c.rgid = c.egid = c.sgid = c.fsgid = gid;
        return 0;
    }
    if (gid != c.rgid && gid != c.sgid)
        return -err::PERM;
    c.egid = c.fsgid = gid;
    return 0;
}

long sys_setreuid(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    proc::Cred& c = my_cred();
    u32 r = static_cast<u32>(a0), e = static_cast<u32>(a1);
    if (c.euid != 0 && ((r != KEEP && r != c.ruid && r != c.euid) ||
                        (e != KEEP && !one_of(e, c.ruid, c.euid, c.suid))))
        return -err::PERM;
    u32 old_ruid = c.ruid;
    if (r != KEEP) c.ruid = r;
    if (e != KEEP) c.euid = e;
    if (r != KEEP || (e != KEEP && e != old_ruid))
        c.suid = c.euid;
    c.fsuid = c.euid;
    return 0;
}

long sys_setregid(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    proc::Cred& c = my_cred();
    u32 r = static_cast<u32>(a0), e = static_cast<u32>(a1);
    if (c.euid != 0 && ((r != KEEP && r != c.rgid && r != c.egid) ||
                        (e != KEEP && !one_of(e, c.rgid, c.egid, c.sgid))))
        return -err::PERM;
    u32 old_rgid = c.rgid;
    if (r != KEEP) c.rgid = r;
    if (e != KEEP) c.egid = e;
    if (r != KEEP || (e != KEEP && e != old_rgid))
        c.sgid = c.egid;
    c.fsgid = c.egid;
    return 0;
}

long sys_setresuid(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    proc::Cred& c = my_cred();
    return set_res(&c.ruid, &c.euid, &c.suid, &c.fsuid, static_cast<u32>(a0), static_cast<u32>(a1),
                   static_cast<u32>(a2), c.euid == 0);
}

long sys_setresgid(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    proc::Cred& c = my_cred();
    return set_res(&c.rgid, &c.egid, &c.sgid, &c.fsgid, static_cast<u32>(a0), static_cast<u32>(a1),
                   static_cast<u32>(a2), c.euid == 0);
}

long sys_setfsuid(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    proc::Cred& c = my_cred();
    u32 old = c.fsuid, u = static_cast<u32>(a0);
    if (c.euid == 0 || one_of(u, c.ruid, c.euid, c.suid) || u == c.fsuid)
        c.fsuid = u;
    return old;
}

long sys_setfsgid(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    proc::Cred& c = my_cred();
    u32 old = c.fsgid, g = static_cast<u32>(a0);
    if (c.euid == 0 || one_of(g, c.rgid, c.egid, c.sgid) || g == c.fsgid)
        c.fsgid = g;
    return old;
}

long sys_getgroups(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    proc::Cred& c = my_cred();
    int size = static_cast<int>(a0);
    if (size < 0)
        return -err::INVAL;
    if (size == 0)
        return c.ngroups;
    if (static_cast<u32>(size) < c.ngroups)
        return -err::INVAL;
    if (long r = uaccess::copy_to(reinterpret_cast<void*>(a1), c.groups, c.ngroups * sizeof(u32)); r < 0)
        return r;
    return c.ngroups;
}

long sys_setgroups(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    proc::Cred& c = my_cred();
    if (c.euid != 0)
        return -err::PERM;
    if (a0 > proc::NGROUPS_MAX)
        return -err::INVAL;
    u32 groups[proc::NGROUPS_MAX];
    if (a0 && uaccess::copy_from(groups, reinterpret_cast<const void*>(a1), a0 * sizeof(u32)) < 0)
        return -err::FAULT;
    memcpy(c.groups, groups, a0 * sizeof(u32));
    c.ngroups = static_cast<u32>(a0);
    return 0;
}

// May the current process signal `target`? (root, or a matching real/saved uid.)
bool may_signal(Process* target, int sig) {
    const proc::Cred& c = proc::cred();
    const proc::Cred& t = target->cred;
    if (c.euid == 0 || c.ruid == t.ruid || c.ruid == t.suid || c.euid == t.ruid || c.euid == t.suid)
        return true;
    return sig == 18 /* SIGCONT */ && target->sid == proc::current()->sid;
}

// ---- process groups and sessions ----

long sys_getpgid(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Process* me = proc::current();
    if (a0 == 0)
        return proc::pgid_nr(me->pgid, me);
    int pgid;
    {
        LockGuard guard(proc::table_lock());
        Process* p = proc::find_visible(static_cast<int>(a0), me);
        if (!p)
            return -err::SRCH;
        pgid = p->pgid;
    }
    return proc::pgid_nr(pgid, me);
}

long sys_getpgrp(SYSCALL_ARGS) { SYSCALL_UNUSED; return proc::pgid_nr(proc::current()->pgid, proc::current()); }

long sys_setpgid(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Process* me = proc::current();
    int pid = a0 ? static_cast<int>(a0) : me->vpid;
    int pgid = a1 ? static_cast<int>(a1) : pid;
    if (pgid < 0)
        return -err::INVAL;
    LockGuard guard(proc::table_lock());
    Process* p = proc::find_visible(pid, me);
    if (!p || (p != me && p->parent != me))
        return -err::SRCH;
    if (p->sid != me->sid)
        return -err::PERM;
    // The group is named by its leader's pid (global here).
    Process* leader = pgid == pid ? p : proc::find_visible(pgid, me);
    p->pgid = leader ? leader->pid : pgid;
    return 0;
}

long sys_getsid(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Process* me = proc::current();
    if (a0 == 0)
        return proc::pgid_nr(me->sid, me);
    int sid;
    {
        LockGuard guard(proc::table_lock());
        Process* p = proc::find_visible(static_cast<int>(a0), me);
        if (!p)
            return -err::SRCH;
        sid = p->sid;
    }
    return proc::pgid_nr(sid, me);
}

long sys_setsid(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Process* me = proc::current();
    if (me->pgid == me->pid)
        return -err::PERM; // already a group leader
    me->sid = me->pgid = me->pid;
    me->ctty = nullptr; // a new session has no controlling terminal until it opens one
    return me->vpid;
}

// ---- fork / exec / wait ----

constexpr u64 SIGCHLD_FLAG = 17;

long sys_fork(SYSCALL_ARGS) { SYSCALL_UNUSED; return proc::do_clone(frame, SIGCHLD_FLAG, 0, 0, 0, 0); }
long sys_vfork(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return proc::do_clone(frame, 0x100 /*CLONE_VM*/ | 0x4000 /*CLONE_VFORK*/ | SIGCHLD_FLAG, 0, 0, 0, 0);
}
// clone(flags, new_sp, parent_tid, child_tid, tls) -- x86_64 argument order
long sys_clone(SYSCALL_ARGS) { SYSCALL_UNUSED; return proc::do_clone(frame, a0, a1, a2, a3, a4); }

// Copies a NULL-terminated array of user strings into one kernel block.
class StringVector {
public:
    ~StringVector() { kfree(m_block); }

    long copy_from_user(u64 user_array) {
        constexpr usize MAX_STRINGS = 4096, MAX_BYTES = 2 * MiB;
        m_block = static_cast<u8*>(kmalloc(MAX_BYTES));
        if (!m_block)
            return -err::NOMEM;
        auto** ptrs = reinterpret_cast<char**>(m_block);
        char* strings = reinterpret_cast<char*>(m_block + (MAX_STRINGS + 1) * sizeof(char*));
        char* end = reinterpret_cast<char*>(m_block + MAX_BYTES);
        usize n = 0;
        if (user_array) {
            for (;; n++) {
                if (n >= MAX_STRINGS)
                    return -err::TOOBIG;
                u64 ptr;
                if (long r = uaccess::get(&ptr, reinterpret_cast<const u64*>(user_array) + n); r < 0)
                    return r;
                if (!ptr)
                    break;
                long len = uaccess::string_from(reinterpret_cast<const char*>(ptr), strings, end - strings);
                if (len < 0)
                    return len == -err::NAMETOOLONG ? -err::TOOBIG : len;
                ptrs[n] = strings;
                strings += len + 1;
            }
        }
        ptrs[n] = nullptr;
        return 0;
    }

    const char* const* get() const { return reinterpret_cast<const char* const*>(m_block); }

private:
    u8* m_block = nullptr;
};

long sys_execve(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    char* path = static_cast<char*>(kmalloc(4096));
    if (!path)
        return -err::NOMEM;
    long r = uaccess::string_from(reinterpret_cast<const char*>(a0), path, 4096);
    StringVector argv, envp;
    if (r >= 0)
        r = argv.copy_from_user(a1);
    if (r >= 0)
        r = envp.copy_from_user(a2);
    if (r >= 0)
        r = proc::do_execve(frame, path, argv.get(), envp.get());
    kfree(path);
    return r;
}

// waitid(idtype, id, siginfo*, options, rusage*): wait with the child's
// status in a siginfo (CLD_EXITED/KILLED/DUMPED, pid, uid, status).
long sys_waitid(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    constexpr int P_ALL = 0, P_PID = 1, P_PGID = 2;
    constexpr int WNOHANG = 1, WEXITED = 4, WNOWAIT = 0x01000000;
    int idtype = static_cast<int>(a0), id = static_cast<int>(a1), options = static_cast<int>(a3);
    if (!(options & WEXITED))
        return -err::INVAL; // stopped/continued children don't exist here (no job-control stops)
    int pid = idtype == P_ALL ? -1 : idtype == P_PID ? id : idtype == P_PGID ? (id ? -id : 0) : -999;
    if (pid == -999)
        return -err::INVAL;
    int status = 0;
    u32 uid = 0;
    long r = proc::do_wait4(pid, &status, options & (WNOHANG | WNOWAIT), &uid);
    if (r < 0)
        return r;
    struct {
        i32 signo, err, code, pad;
        i32 pid;
        u32 uid;
        i32 status;
        i32 pad2;
        u8 rest[96];
    } info{};
    static_assert(sizeof(info) == 128);
    if (r > 0) {
        info.signo = 17; // SIGCHLD
        info.pid = static_cast<i32>(r);
        info.uid = uid;
        if ((status & 0x7F) == 0) {
            info.code = 1; // CLD_EXITED
            info.status = (status >> 8) & 0xFF;
        } else {
            info.code = (status & 0x80) ? 3 : 2; // CLD_DUMPED / CLD_KILLED
            info.status = status & 0x7F;
        }
    }
    if (a2)
        if (long e = uaccess::put(reinterpret_cast<decltype(info)*>(a2), info); e < 0)
            return e;
    if (a4) {
        u8 zeros[144] = {};
        uaccess::copy_to(reinterpret_cast<void*>(a4), zeros, sizeof(zeros));
    }
    return 0;
}

long sys_wait4(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    int status = 0;
    long r = proc::do_wait4(static_cast<int>(a0), &status, static_cast<int>(a2));
    if (r > 0 && a1) {
        if (long e = uaccess::put(reinterpret_cast<i32*>(a1), status); e < 0)
            return e;
    }
    if (r > 0 && a3) { // struct rusage: no accounting yet, report zeros
        u8 zeros[144] = {};
        uaccess::copy_to(reinterpret_cast<void*>(a3), zeros, sizeof(zeros));
    }
    return r;
}

// ---- signals ----

long sys_kill(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    int pid = static_cast<int>(a0);
    int sig = static_cast<int>(a1);
    if (sig < 0 || sig > proc::MAX_SIGNALS)
        return -err::INVAL;
    Process* me = proc::current();
    if (pid == 0 || pid < -1) {
        int pgid = pid == 0 ? me->pgid : proc::pgid_global(-pid, me);
        if (pgid < 0)
            return -err::SRCH;
        signal::send_to_group(pgid, sig);
        return 0;
    }
    if (pid == -1) { // everyone (we can see) but init and ourselves (that we may signal)
        proc::for_each([](Process* p, void* arg) {
            int s = static_cast<int>(reinterpret_cast<uptr>(arg));
            Process* self = proc::current();
            if (p != self && p->vpid != 1 && s && proc::visible(p, self) && may_signal(p, s))
                signal::send(p, s, signal::SI_USER);
        }, reinterpret_cast<void*>(static_cast<uptr>(sig)));
        return 0;
    }
    LockGuard guard(proc::table_lock());
    Process* p = proc::find_visible(pid, me);
    if (!p || p->state == proc::State::Zombie)
        return -err::SRCH;
    if (!may_signal(p, sig))
        return -err::PERM;
    if (sig)
        signal::send(p, sig, signal::SI_USER);
    return 0;
}

long sys_tgkill(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Process* me = proc::current();
    LockGuard guard(proc::table_lock());
    sched::Thread* t = proc::find_visible_thread(static_cast<int>(a1), me);
    if (!t || !t->process || (me->pidns ? t->process->vpid : t->process->pid) != static_cast<int>(a0))
        return -err::SRCH;
    if (!may_signal(t->process, static_cast<int>(a2)))
        return -err::PERM;
    if (a2)
        signal::send_thread(t, static_cast<int>(a2), signal::SI_TKILL);
    return 0;
}

long sys_tkill(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    LockGuard guard(proc::table_lock());
    sched::Thread* t = proc::find_visible_thread(static_cast<int>(a0), proc::current());
    if (!t)
        return -err::SRCH;
    if (t->process && !may_signal(t->process, static_cast<int>(a1)))
        return -err::PERM;
    if (a1)
        signal::send_thread(t, static_cast<int>(a1), signal::SI_TKILL);
    return 0;
}

long sys_rt_sigreturn(SYSCALL_ARGS) { SYSCALL_UNUSED; return signal::sigreturn(frame); }

long sys_pause(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    sched::WaitQueue wq;
    wq.wait_until([] { return signal::interrupted(); });
    return -err::INTR;
}

long sys_rt_sigsuspend(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    sched::Thread* t = sched::current();
    u64 mask;
    if (long r = uaccess::get(&mask, reinterpret_cast<const u64*>(a0)); r < 0)
        return r;
    u64 old = t->sigmask;
    t->sigmask = mask & ~((1ull << 8) | (1ull << 18)); // never block SIGKILL / SIGSTOP
    sched::WaitQueue wq;
    wq.wait_until([] { return signal::interrupted(); });
    // The handler must run with the temporary mask; restored when it returns.
    // TODO: save `old` in the signal frame instead of restoring it here.
    signal::deliver(frame);
    t->sigmask = old;
    return -err::INTR;
}

// rt_sigtimedwait(set, info, timeout, sigsetsize): take one of the signals in
// `set` (normally blocked by the caller) off the pending set, waiting for one
// if needed (sigwait, sigwaitinfo, sigtimedwait; dbus and xcape use it).
// Other, unblocked signals interrupt the wait (EINTR); a timeout gives EAGAIN.
long sys_rt_sigtimedwait(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a3 != sizeof(u64))
        return -err::INVAL;
    u64 set;
    if (long r = uaccess::get(&set, reinterpret_cast<const u64*>(a0)); r < 0)
        return r;
    set &= ~((1ull << 8) | (1ull << 18)); // SIGKILL / SIGSTOP can't be waited for
    u64 deadline = 0;
    if (a2) {
        struct { i64 sec, nsec; } ts;
        if (long r = uaccess::copy_from(&ts, reinterpret_cast<void*>(a2), sizeof(ts)); r < 0)
            return r;
        if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1'000'000'000)
            return -err::INVAL;
        deadline = clock::now_ns() + static_cast<u64>(ts.sec) * 1'000'000'000 + static_cast<u64>(ts.nsec);
    }
    int sig = signal::dequeue(set);
    if (!sig) {
        sched::WaitQueue wq; // woken by the sender (it wakes every sleeping thread)
        auto ready = [&] { return (signal::pending_set() & set) != 0 || signal::interrupted(); };
        for (;;) {
            if (a2) {
                u64 now = clock::now_ns();
                if (now >= deadline || !wq.wait_until_timeout(ready, deadline - now))
                    break;
            } else {
                wq.wait_until(ready);
            }
            if ((sig = signal::dequeue(set)) != 0)
                break;
            if (signal::interrupted())
                return -err::INTR;
        }
        if (!sig)
            sig = signal::dequeue(set);
    }
    if (!sig)
        return -err::AGAIN;
    if (a1) {
        // siginfo_t (128 bytes): signo, errno, code, then pid and uid for a kill.
        struct SigInfo {
            i32 signo, err, code, pad;
            i32 pid;
            u32 uid;
            u8 rest[104];
        } info{};
        Process* p = proc::current();
        info.signo = sig;
        info.code = p->sig_code[sig];
        info.pid = p->sig_pid[sig];
        info.uid = p->sig_uid[sig];
        if (long r = uaccess::put(reinterpret_cast<SigInfo*>(a1), info); r < 0)
            return r;
    }
    return sig;
}

long sys_sched_yield(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    sched::yield();
    return 0;
}

long sys_arch_prctl(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    constexpr u64 ARCH_SET_GS = 0x1001, ARCH_SET_FS = 0x1002, ARCH_GET_FS = 0x1003, ARCH_GET_GS = 0x1004;
    sched::Thread* t = sched::current();
    switch (a0) {
    case ARCH_SET_FS: {
        if (a1 >= mm::USER_END)
            return -err::PERM;
        // A selector loaded in %fs (Wine's 32-bit TEB) gives way to the base,
        // as on Linux; no context switch in between (it would reload it).
        InterruptGuard guard;
        compat_segments::clear_fs();
        t->fs_base = a1;
        arch::wrmsr(MSR_FS_BASE, a1);
        return 0;
    }
    case ARCH_SET_GS: {
        if (a1 >= mm::USER_END)
            return -err::PERM;
        InterruptGuard guard;
        compat_segments::clear_gs();
        t->gs_base = a1;
        arch::wrmsr(MSR_KERNEL_GS_BASE, a1); // swapped in on return to user
        return 0;
    }
    case ARCH_GET_FS:
        return uaccess::put(reinterpret_cast<u64*>(a1), t->fs_base);
    case ARCH_GET_GS:
        return uaccess::put(reinterpret_cast<u64*>(a1), t->gs_base);
    default:
        return -err::INVAL;
    }
}

long sys_set_tid_address(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    sched::current()->clear_child_tid = a0;
    return sched::current()->vtid;
}

long sys_set_robust_list(SYSCALL_ARGS) { SYSCALL_UNUSED; return 0; }

// Optional kernel features that libc probes for and copes without (rseq,
// clone3 falls back to clone). Answer quietly instead of logging a warning.
long sys_quiet_nosys(SYSCALL_ARGS) { SYSCALL_UNUSED; return -err::NOSYS; }

struct UtsName {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};

long sys_uname(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    UtsName u{};
    // Report "Linux": Linux programs (and libc) branch on this. The release
    // string says what's really underneath.
    snprintf(u.sysname, 65, "Linux");
    const proc::Uts* uts = proc::current()->uts ? proc::current()->uts : proc::system_uts();
    snprintf(u.nodename, 65, "%s", uts->nodename);
    snprintf(u.release, 65, "6.12.0-syluxium");
    snprintf(u.version, 65, "#1 SMP Syluxium " __DATE__);
    snprintf(u.machine, 65, "x86_64");
    snprintf(u.domainname, 65, "%s", uts->domainname);
    return uaccess::put(reinterpret_cast<UtsName*>(a0), u);
}

long sys_rt_sigaction(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    int sig = static_cast<int>(a0);
    if (sig < 1 || sig > proc::MAX_SIGNALS)
        return -err::INVAL;
    Process* p = proc::current();
    if (a2) {
        if (long r = uaccess::put(reinterpret_cast<proc::SigAction*>(a2), p->sigactions[sig]); r < 0)
            return r;
    }
    if (a1) {
        proc::SigAction act;
        if (long r = uaccess::get(&act, reinterpret_cast<const proc::SigAction*>(a1)); r < 0)
            return r;
        p->sigactions[sig] = act;
    }
    return 0;
}

long sys_rt_sigprocmask(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    constexpr int SIG_BLOCK = 0, SIG_UNBLOCK = 1, SIG_SETMASK = 2;
    sched::Thread* t = sched::current();
    if (a2) {
        if (long r = uaccess::put(reinterpret_cast<u64*>(a2), t->sigmask); r < 0)
            return r;
    }
    if (a1) {
        u64 set;
        if (long r = uaccess::get(&set, reinterpret_cast<const u64*>(a1)); r < 0)
            return r;
        switch (static_cast<int>(a0)) {
        case SIG_BLOCK:   t->sigmask |= set; break;
        case SIG_UNBLOCK: t->sigmask &= ~set; break;
        case SIG_SETMASK: t->sigmask = set; break;
        default:          return -err::INVAL;
        }
    }
    return 0;
}

// rt_sigpending(set, sigsetsize): the blocked signals waiting for this thread
// (its own and the process's), as Linux reports them (sudo asks).
long sys_rt_sigpending(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a1 != sizeof(u64))
        return -err::INVAL;
    sched::Thread* t = sched::current();
    Process* p = proc::current();
    u64 pending = __atomic_load_n(&t->sig_pending, __ATOMIC_RELAXED);
    if (p)
        pending |= __atomic_load_n(&p->sig_pending, __ATOMIC_RELAXED);
    return uaccess::put(reinterpret_cast<u64*>(a0), pending & t->sigmask);
}

// stack_t on x86_64: { void* ss_sp; int ss_flags; size_t ss_size; }
long sys_sigaltstack(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    struct StackT {
        u64 ss_sp;
        i32 ss_flags;
        i32 pad;
        u64 ss_size;
    };
    signal::AltStack ns, os;
    if (a0) {
        StackT u;
        if (uaccess::get(&u, reinterpret_cast<const StackT*>(a0)) < 0)
            return -err::FAULT;
        ns = signal::AltStack{u.ss_sp, static_cast<u32>(u.ss_flags), u.ss_size};
    }
    if (long r = signal::sigaltstack(a0 ? &ns : nullptr, &os, frame->rsp); r < 0)
        return r;
    if (a1)
        return uaccess::put(reinterpret_cast<StackT*>(a1), StackT{os.sp, static_cast<i32>(os.flags), 0, os.size});
    return 0;
}

struct RLimit {
    u64 cur, max;
};

RLimit default_limit(u32 resource) {
    constexpr u64 INF = ~0ull;
    switch (resource) {
    case 3:  return {8 * MiB, INF};          // RLIMIT_STACK
    case 7:  return {1024, proc::MAX_FDS};   // RLIMIT_NOFILE
    default: return {INF, INF};
    }
}

long sys_prlimit64(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a0 != 0 && static_cast<int>(a0) != proc::current()->pid)
        return -err::SRCH;
    if (a3)
        return uaccess::put(reinterpret_cast<RLimit*>(a3), default_limit(static_cast<u32>(a1)));
    return 0; // setting limits: accepted, not enforced yet
}

long sys_getrlimit(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return uaccess::put(reinterpret_cast<RLimit*>(a1), default_limit(static_cast<u32>(a0)));
}

long sys_getrandom(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    usize len = min<u64>(a1, 32 * MiB);
    if (long r = uaccess::check(reinterpret_cast<void*>(a0), len, true); r < 0)
        return r;
    protect::UserAccess window;
    random::fill(reinterpret_cast<void*>(a0), len);
    return static_cast<long>(len);
}

struct SysInfo {
    i64 uptime;
    u64 loads[3];
    u64 totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    u16 procs;
    u16 pad;
    u64 totalhigh, freehigh;
    u32 mem_unit;
    char reserved[4];
};

long sys_sysinfo(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    auto st = pmm::stats();
    SysInfo info{};
    info.uptime = static_cast<i64>(clock::now_ns() / 1'000'000'000);
    info.totalram = st.total_pages * 4096;
    info.freeram = st.free_pages * 4096;
    info.procs = static_cast<u16>(proc::count());
    info.mem_unit = 1;
    return uaccess::put(reinterpret_cast<SysInfo*>(a0), info);
}

// syslog(2) / klogctl: what dmesg uses.
// getresuid/getresgid: real, effective and saved IDs (everyone is root for now).
long put_ids(u64 pr, u64 pe, u64 ps, u32 r, u32 e, u32 sv) {
    if (long x = uaccess::put(reinterpret_cast<u32*>(pr), r); x < 0) return x;
    if (long x = uaccess::put(reinterpret_cast<u32*>(pe), e); x < 0) return x;
    return uaccess::put(reinterpret_cast<u32*>(ps), sv);
}
long sys_getresuid(SYSCALL_ARGS) { SYSCALL_UNUSED; proc::Cred& c = my_cred(); return put_ids(a0, a1, a2, c.ruid, c.euid, c.suid); }
long sys_getresgid(SYSCALL_ARGS) { SYSCALL_UNUSED; proc::Cred& c = my_cred(); return put_ids(a0, a1, a2, c.rgid, c.egid, c.sgid); }

// capget/capset: everything runs as root with every capability.
long sys_capget(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    struct Header {
        u32 version;
        i32 pid;
    } h;
    if (long r = uaccess::get(&h, reinterpret_cast<Header*>(a0)); r < 0)
        return r;
    constexpr u32 V3 = 0x20080522;
    if (h.version != V3 && h.version != 0x20071026 && h.version != 0x19980330) {
        h.version = V3; // tell the caller which version we speak
        uaccess::put(reinterpret_cast<Header*>(a0), h);
        return a1 ? -err::INVAL : 0;
    }
    if (a1) {
        u32 data[6] = {0xFFFFFFFF, 0xFFFFFFFF, 0, 0x000001FF, 0x000001FF, 0}; // effective, permitted, inheritable x2
        int words = h.version == 0x19980330 ? 3 : 6;
        return uaccess::copy_to(reinterpret_cast<void*>(a1), data, static_cast<usize>(words) * 4);
    }
    return 0;
}

long sys_capset(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return 0;
}

// Scheduling policy: everything runs as SCHED_OTHER (priority 0); asking
// for a real-time policy is accepted for root and ignored by the scheduler.
long sys_sched_getscheduler(SYSCALL_ARGS) { SYSCALL_UNUSED; return 0; } // SCHED_OTHER
long sys_sched_setscheduler(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a1 != 0 && a1 != 3 && a1 != 5 && !proc::capable()) // OTHER, BATCH, IDLE are always fine
        return -err::PERM;
    return 0;
}
long sys_sched_getparam(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    i32 priority = 0;
    return uaccess::put(reinterpret_cast<i32*>(a1), priority);
}
long sys_sched_setparam(SYSCALL_ARGS) { SYSCALL_UNUSED; return 0; }
long sys_sched_priority_max(SYSCALL_ARGS) { SYSCALL_UNUSED; return (a0 == 1 || a0 == 2) ? 99 : 0; } // FIFO/RR: 99
long sys_sched_priority_min(SYSCALL_ARGS) { SYSCALL_UNUSED; return (a0 == 1 || a0 == 2) ? 1 : 0; }
long sys_sched_rr_get_interval(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    i64 ts[2] = {0, 10'000'000}; // 10 ms
    return uaccess::copy_to(reinterpret_cast<void*>(a1), ts, sizeof(ts));
}

// getpriority/setpriority: nice values are accepted but don't change scheduling yet.
long sys_getpriority(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return 20; // the kernel returns 20 - nice
}

long sys_setpriority(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return 0;
}

// close_range(first, last, flags): what glibc's closefrom() and many daemons use.
long sys_close_range(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    constexpr u64 CLOSE_RANGE_UNSHARE = 2, CLOSE_RANGE_CLOEXEC = 4;
    if (a2 & ~(CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC) || a0 > a1)
        return -err::INVAL;
    proc::Process* p = proc::current();
    u64 last = a1 < static_cast<u64>(proc::MAX_FDS) ? a1 : proc::MAX_FDS - 1;
    for (u64 fd = a0; fd <= last; fd++) {
        if (a2 & CLOSE_RANGE_CLOEXEC) {
            if (p->fds[fd])
                p->cloexec[fd] = true;
        } else if (p->fds[fd]) {
            proc::fd_close(p, static_cast<int>(fd));
        }
    }
    return 0;
}

// reboot(magic1, magic2, cmd): power off / restart / halt (root only).
// Disks are flushed and journaled file systems marked clean first.
long sys_reboot(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    constexpr u64 MAGIC1 = 0xfee1dead;
    constexpr u32 RESTART = 0x01234567, HALT = 0xCDEF0123, POWER_OFF = 0x4321FEDC, CAD_ON = 0x89ABCDEF,
                  CAD_OFF = 0, RESTART2 = 0xA1B2C3D4;
    if (!proc::capable())
        return -err::PERM;
    u32 m2 = static_cast<u32>(a1);
    if ((a0 & 0xFFFFFFFF) != MAGIC1 || (m2 != 672274793 && m2 != 85072278 && m2 != 369367448 && m2 != 537993216))
        return -err::INVAL;
    u32 cmd = static_cast<u32>(a2);
    if (cmd == CAD_ON || cmd == CAD_OFF)
        return 0;
    if (cmd != RESTART && cmd != RESTART2 && cmd != HALT && cmd != POWER_OFF)
        return -err::INVAL;
    klog::info("System %s: flushing disks...", cmd == POWER_OFF ? "power off" : cmd == HALT ? "halt" : "restart");
    bootlog::save();
    block::sync_all();
    ext2::shutdown_all();
    block::sync_all();
    if (cmd == RESTART || cmd == RESTART2)
        acpi::restart();
    if (cmd == POWER_OFF)
        acpi::power_off();
    klog::ok("System halted: it's safe to turn the computer off.");
    arch::cli();
    for (;;)
        arch::hlt();
}

long sys_syslog(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    int type = static_cast<int>(a0);
    switch (type) {
    case 2: case 3: case 4: { // READ, READ_ALL, READ_CLEAR
        usize len = static_cast<usize>(a2);
        if (static_cast<i64>(a2) < 0)
            return -err::INVAL;
        if (len > 128 * 1024)
            len = 128 * 1024;
        char* tmp = static_cast<char*>(kmalloc(len ? len : 1));
        if (!tmp)
            return -err::NOMEM;
        usize n = klog::read_buffer(tmp, len);
        long r = uaccess::copy_to(reinterpret_cast<void*>(a1), tmp, n);
        kfree(tmp);
        if (r < 0)
            return r;
        if (type == 4)
            klog::clear_buffer();
        return static_cast<long>(n);
    }
    case 5: klog::clear_buffer(); return 0;   // CLEAR
    case 0: case 1: case 6: case 7: case 8: return 0; // close/open/console level
    case 9: return static_cast<long>(klog::buffer_length()); // SIZE_UNREAD
    case 10: return 128 * 1024;                              // SIZE_BUFFER
    default: return -err::INVAL;
    }
}

long sys_prctl(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    constexpr int PR_SET_PDEATHSIG = 1, PR_GET_PDEATHSIG = 2, PR_GET_DUMPABLE = 3, PR_SET_DUMPABLE = 4,
                  PR_SET_NAME = 15, PR_GET_NAME = 16, PR_SET_NO_NEW_PRIVS = 38, PR_GET_NO_NEW_PRIVS = 39;
    sched::Thread* t = sched::current();
    switch (static_cast<int>(a0)) {
    case PR_SET_NAME: { // thread name (shown in /proc/<pid>/task/<tid>/comm, Task Manager)
        char name[16];
        long n = uaccess::string_from(reinterpret_cast<const char*>(a1), name, sizeof(name));
        if (n == -err::NAMETOOLONG) { // longer names are truncated, not an error
            if (uaccess::copy_from(name, reinterpret_cast<const void*>(a1), 15) < 0)
                return -err::FAULT;
            name[15] = 0;
        } else if (n < 0) {
            return n;
        }
        snprintf(t->name, sizeof(t->name), "%s", name);
        return 0;
    }
    case PR_GET_NAME: {
        char name[16] = {};
        snprintf(name, sizeof(name), "%s", t->name);
        return uaccess::copy_to(reinterpret_cast<void*>(a1), name, 16);
    }
    case PR_GET_DUMPABLE: return 1;
    case PR_SET_DUMPABLE: case PR_SET_PDEATHSIG: case PR_SET_NO_NEW_PRIVS: return 0;
    case PR_GET_PDEATHSIG: return uaccess::put(reinterpret_cast<i32*>(a1), 0);
    case PR_GET_NO_NEW_PRIVS: return 0;
    // Capabilities (libcap-ng, daemons dropping privileges): every process is
    // root with every capability, so these succeed without changing anything.
    case 7:  return 0;                       // PR_GET_KEEPCAPS
    case 8:  return a1 > 1 ? -err::INVAL : 0; // PR_SET_KEEPCAPS
    case 23: return a1 <= 40 ? 1 : -err::INVAL; // PR_CAPBSET_READ: in the bounding set
    case 24: return 0;                       // PR_CAPBSET_DROP
    case 27: return 0;                       // PR_GET_SECUREBITS
    case 28: return 0;                       // PR_SET_SECUREBITS
    case 47: return a1 == 1 /* IS_SET */ ? 0 : 0; // PR_CAP_AMBIENT
    case 36: case 37: return 0;              // PR_SET/GET_CHILD_SUBREAPER
    case 22: return 0;                       // PR_SET_SECCOMP / 21 GET: no filters
    case 21: return 0;
    default: return -err::INVAL;
    }
}

long sys_getcpu(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    u32 cpu = this_cpu()->index;
    if (a0 && uaccess::put(reinterpret_cast<u32*>(a0), cpu) < 0)
        return -err::FAULT;
    if (a1 && uaccess::put(reinterpret_cast<u32*>(a1), 0u) < 0) // NUMA node
        return -err::FAULT;
    return 0;
}

long sys_sched_getaffinity(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    u32 cpus = percpu::online_count();
    u8 mask[128] = {};
    for (u32 i = 0; i < cpus && i < sizeof(mask) * 8; i++)
        mask[i / 8] |= 1 << (i % 8);
    usize n = min<u64>(a1, sizeof(mask));
    if (long r = uaccess::copy_to(reinterpret_cast<void*>(a2), mask, n); r < 0)
        return r;
    return static_cast<long>(n);
}

// We don't pin threads to CPUs: accept any mask that names an online CPU
// (what Linux requires) and keep scheduling everywhere. wineserver and
// some games set their affinity.
long sys_sched_setaffinity(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (static_cast<i32>(a0) < 0)
        return -err::INVAL;
    if (a0) {
        LockGuard guard(proc::table_lock());
        if (!proc::find_visible_thread(static_cast<int>(a0), proc::current()))
            return -err::SRCH;
    }
    u32 cpus = percpu::online_count();
    u8 mask[128] = {};
    usize n = min<u64>(a1, sizeof(mask));
    if (long r = uaccess::copy_from(mask, reinterpret_cast<const void*>(a2), n); r < 0)
        return r;
    for (u32 i = 0; i < cpus && i < n * 8; i++)
        if (mask[i / 8] & (1 << (i % 8)))
            return 0;
    return -err::INVAL;
}

long sys_futex(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    constexpr u32 FUTEX_WAIT = 0, FUTEX_WAKE = 1, FUTEX_REQUEUE = 3, FUTEX_CMP_REQUEUE = 4,
                  FUTEX_WAIT_BITSET = 9, FUTEX_WAKE_BITSET = 10;
    constexpr u32 FUTEX_PRIVATE_FLAG = 128, FUTEX_CLOCK_REALTIME = 256;
    u64 uaddr = a0;
    u32 op = static_cast<u32>(a1);
    u32 cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    bool is_private = op & FUTEX_PRIVATE_FLAG;
    u32 val = static_cast<u32>(a2);

    switch (cmd) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET: {
        u64 deadline = 0;
        if (a3) {
            struct { i64 sec, nsec; } ts;
            if (long r = uaccess::copy_from(&ts, reinterpret_cast<void*>(a3), sizeof(ts)); r < 0)
                return r;
            if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1'000'000'000)
                return -err::INVAL;
            u64 t = static_cast<u64>(ts.sec) * 1'000'000'000 + static_cast<u64>(ts.nsec);
            if (cmd == FUTEX_WAIT) {
                deadline = clock::now_ns() + t; // relative timeout
            } else if (op & FUTEX_CLOCK_REALTIME) {
                u64 real_now = rtc::realtime_ns(), mono_now = clock::now_ns();
                deadline = t > real_now ? mono_now + (t - real_now) : mono_now; // absolute, wall clock
            } else {
                deadline = t; // absolute, CLOCK_MONOTONIC (our clock::now_ns)
            }
            if (deadline == 0)
                deadline = 1; // 0 means "no timeout"
        }
        u32 bitset = cmd == FUTEX_WAIT ? ~0u : static_cast<u32>(a5);
        return futex::wait(uaddr, val, deadline, bitset, is_private);
    }
    case FUTEX_WAKE:
        return futex::wake(uaddr, static_cast<int>(val), ~0u, is_private);
    case FUTEX_WAKE_BITSET:
        return futex::wake(uaddr, static_cast<int>(val), static_cast<u32>(a5), is_private);
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE:
        return futex::requeue(uaddr, a4, static_cast<int>(val), static_cast<int>(a3), cmd == FUTEX_CMP_REQUEUE,
                              static_cast<u32>(a5), is_private);
    default:
        return -err::NOSYS;
    }
}

} // namespace

// ---- containers: host names, namespaces ----

constexpr u64 CLONE_NEWNS = 0x20000, CLONE_NEWCGROUP = 0x02000000, CLONE_NEWUTS = 0x04000000,
              CLONE_NEWIPC = 0x08000000, CLONE_NEWUSER = 0x10000000, CLONE_NEWPID = 0x20000000,
              CLONE_NEWNET = 0x40000000, CLONE_NEWTIME = 0x80, CLONE_FS = 0x200, CLONE_FILES = 0x400,
              CLONE_SYSVSEM = 0x40000;

// The calling process's own UTS namespace (copied on first write after unshare).
proc::Uts* own_uts() {
    Process* p = proc::current();
    if (!p->uts) {
        p->uts = proc::system_uts();
        proc::uts_get(p->uts);
    }
    return p->uts;
}

long set_name(u64 user, u64 len, bool host) {
    if (!proc::capable())
        return -err::PERM;
    if (len > 64)
        return -err::INVAL;
    char name[65] = {};
    if (len && uaccess::copy_from(name, reinterpret_cast<const void*>(user), len) < 0)
        return -err::FAULT;
    proc::Uts* u = own_uts();
    snprintf(host ? u->nodename : u->domainname, 65, "%s", name);
    return 0;
}

long sys_sethostname(SYSCALL_ARGS) { SYSCALL_UNUSED; return set_name(a0, a1, true); }
long sys_setdomainname(SYSCALL_ARGS) { SYSCALL_UNUSED; return set_name(a0, a1, false); }

// unshare(): a private UTS namespace (host name) is real. Mount, IPC,
// network, PID, cgroup and user namespaces are accepted but stay shared with
// the rest of the system (containers still get their own "/" through
// chroot/pivot_root).
long sys_unshare(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    u64 flags = a0;
    constexpr u64 known = CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID |
                          CLONE_NEWNET | CLONE_NEWTIME | CLONE_FS | CLONE_FILES | CLONE_SYSVSEM;
    if (flags & ~known)
        return -err::INVAL;
    if ((flags & (known & ~(CLONE_FS | CLONE_FILES | CLONE_SYSVSEM | CLONE_NEWUSER))) && !proc::capable())
        return -err::PERM;
    if (flags & CLONE_NEWNS) { // a private copy of the mount table
        Process* p = proc::current();
        void* old = p->mntns;
        p->mntns = vfs::mount_ns_copy(old);
        vfs::mount_ns_put(old);
    }
    if (flags & CLONE_NEWUTS) {
        Process* p = proc::current();
        auto* u = new proc::Uts(*(p->uts ? p->uts : proc::system_uts()));
        u->refs = 1;
        if (p->uts)
            proc::uts_put(p->uts);
        p->uts = u;
    }
    return 0;
}

// setns(fd, type): fd is /proc/<pid>/ns/<type> (procfs). Entering a
// process's "mnt" namespace takes its root (containers: their "/"), "uts" its
// host name; the others are shared anyway.
long sys_setns(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (!proc::capable())
        return -err::PERM;
    vfs::File* f = proc::fd_get(proc::current(), static_cast<int>(a0));
    if (!f)
        return -err::BADF;
    int pid = 0;
    char kind[16] = {};
    long r = proc::ns_file_target(f, &pid, kind, sizeof(kind));
    vfs::file_put(f);
    if (r < 0)
        return r;
    Process* me = proc::current();
    LockGuard guard(proc::table_lock());
    Process* target = proc::find(pid);
    if (!target)
        return -err::SRCH;
    if (!strcmp(kind, "mnt")) {
        vfs::mount_ns_get(target->mntns);
        vfs::mount_ns_put(me->mntns);
        me->mntns = target->mntns;
        me->root = target->root;
        me->cwd = target->root ? target->root : vfs::root();
        snprintf(me->cwd_path, sizeof(me->cwd_path), "/");
    } else if (!strcmp(kind, "pid")) {
        // Our children (not we) will live in the target's PID namespace.
        if (me->pidns)
            return target->pidns == me->pidns ? 0 : -err::INVAL;
        if (me->child_pidns && __atomic_sub_fetch(&me->child_pidns->refs, 1, __ATOMIC_ACQ_REL) == 0)
            delete me->child_pidns;
        me->child_pidns = target->pidns;
        if (me->child_pidns)
            __atomic_add_fetch(&me->child_pidns->refs, 1, __ATOMIC_RELAXED);
    } else if (!strcmp(kind, "uts")) {
        proc::Uts* u = target->uts ? target->uts : proc::system_uts();
        proc::uts_get(u);
        if (me->uts)
            proc::uts_put(me->uts);
        me->uts = u;
    }
    return 0;
}

long sys_personality(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    // PER_LINUX (with flags such as ADDR_NO_RANDOMIZE) only; 0xffffffff queries.
    Process* me = proc::current();
    u32 old = me->personality;
    if (a0 == 0xffffffff)
        return old;
    if ((a0 & 0xff) != 0)
        return -err::INVAL;
    me->personality = static_cast<u32>(a0);
    return old;
}

// Memory locking: every page stays resident anyway (no swap).
long sys_mlock_ok(SYSCALL_ARGS) { SYSCALL_UNUSED; return 0; }
long sys_ioprio_get(SYSCALL_ARGS) { SYSCALL_UNUSED; return (2 << 13) | 4; } // best effort, priority 4
long sys_membarrier(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    constexpr u64 QUERY = 0, GLOBAL = 1, PRIVATE_EXPEDITED = 8, REGISTER_PRIVATE_EXPEDITED = 16;
    switch (a0) {
    case QUERY: return GLOBAL | PRIVATE_EXPEDITED | REGISTER_PRIVATE_EXPEDITED;
    case GLOBAL: case PRIVATE_EXPEDITED: case REGISTER_PRIVATE_EXPEDITED:
        __atomic_thread_fence(__ATOMIC_SEQ_CST); // syscall entry/exit serializes the other CPUs' threads
        return 0;
    default: return -err::INVAL;
    }
}

void register_process() {
    set(135, sys_personality, "personality");
    set(149, sys_mlock_ok, "mlock");
    set(150, sys_mlock_ok, "munlock");
    set(151, sys_mlock_ok, "mlockall");
    set(152, sys_mlock_ok, "munlockall");
    set(325, sys_mlock_ok, "mlock2");
    set(170, sys_sethostname, "sethostname");
    set(171, sys_setdomainname, "setdomainname");
    set(251, sys_mlock_ok, "ioprio_set");
    set(252, sys_ioprio_get, "ioprio_get");
    set(272, sys_unshare, "unshare");
    set(308, sys_setns, "setns");
    set(324, sys_membarrier, "membarrier");
    set(13, sys_rt_sigaction, "rt_sigaction");
    set(14, sys_rt_sigprocmask, "rt_sigprocmask");
    set(127, sys_rt_sigpending, "rt_sigpending");
    set(24, sys_sched_yield, "sched_yield");
    set(39, sys_getpid, "getpid");
    set(60, sys_exit, "exit");
    set(63, sys_uname, "uname");
    set(97, sys_getrlimit, "getrlimit");
    set(99, sys_sysinfo, "sysinfo");
    set(103, sys_syslog, "syslog");
    set(169, sys_reboot, "reboot");
    set(247, sys_waitid, "waitid");
    set(125, sys_capget, "capget");
    set(126, sys_capset, "capset");
    set(140, sys_getpriority, "getpriority");
    set(142, sys_sched_setparam, "sched_setparam");
    set(143, sys_sched_getparam, "sched_getparam");
    set(144, sys_sched_setscheduler, "sched_setscheduler");
    set(145, sys_sched_getscheduler, "sched_getscheduler");
    set(146, sys_sched_priority_max, "sched_get_priority_max");
    set(147, sys_sched_priority_min, "sched_get_priority_min");
    set(148, sys_sched_rr_get_interval, "sched_rr_get_interval");
    set(141, sys_setpriority, "setpriority");
    set(436, sys_close_range, "close_range");
    set(117, sys_setresuid, "setresuid");
    set(118, sys_getresuid, "getresuid");
    set(119, sys_setresgid, "setresgid");
    set(120, sys_getresgid, "getresgid");
    set(115, sys_getgroups, "getgroups");
    set(116, sys_setgroups, "setgroups");
    set(102, sys_getuid, "getuid");
    set(104, sys_getgid, "getgid");
    set(105, sys_setuid, "setuid");
    set(106, sys_setgid, "setgid");
    set(107, sys_geteuid, "geteuid");
    set(108, sys_getegid, "getegid");
    set(113, sys_setreuid, "setreuid");
    set(114, sys_setregid, "setregid");
    set(122, sys_setfsuid, "setfsuid");
    set(123, sys_setfsgid, "setfsgid");
    set(109, sys_setpgid, "setpgid");
    set(110, sys_getppid, "getppid");
    set(111, sys_getpgrp, "getpgrp");
    set(112, sys_setsid, "setsid");
    set(121, sys_getpgid, "getpgid");
    set(124, sys_getsid, "getsid");
    set(15, sys_rt_sigreturn, "rt_sigreturn");
    set(34, sys_pause, "pause");
    set(56, sys_clone, "clone");
    set(57, sys_fork, "fork");
    set(58, sys_vfork, "vfork");
    set(59, sys_execve, "execve");
    set(61, sys_wait4, "wait4");
    set(62, sys_kill, "kill");
    set(130, sys_rt_sigsuspend, "rt_sigsuspend");
    set(128, sys_rt_sigtimedwait, "rt_sigtimedwait");
    set(200, sys_tkill, "tkill");
    set(234, sys_tgkill, "tgkill");
    set(435, sys_quiet_nosys, "clone3");
    set(131, sys_sigaltstack, "sigaltstack");
    set(158, sys_arch_prctl, "arch_prctl");
    set(186, sys_gettid, "gettid");
    set(202, sys_futex, "futex");
    set(203, sys_sched_setaffinity, "sched_setaffinity");
    set(204, sys_sched_getaffinity, "sched_getaffinity");
    set(218, sys_set_tid_address, "set_tid_address");
    set(231, sys_exit_group, "exit_group");
    set(273, sys_set_robust_list, "set_robust_list");
    set(302, sys_prlimit64, "prlimit64");
    set(318, sys_getrandom, "getrandom");
    set(309, sys_getcpu, "getcpu");
    set(157, sys_prctl, "prctl");
    set(334, sys_quiet_nosys, "rseq");
}

} // namespace syscall
