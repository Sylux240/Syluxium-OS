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

long sys_getpid(SYSCALL_ARGS) { SYSCALL_UNUSED; return proc::current()->pid; }
long sys_getppid(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    LockGuard guard(proc::table_lock());
    Process* parent = proc::current()->parent;
    return parent ? parent->pid : 1; // orphans belong to "init", as on Linux
}
long sys_gettid(SYSCALL_ARGS) { SYSCALL_UNUSED; return sched::current()->user_tid; }
long sys_getid(SYSCALL_ARGS) { SYSCALL_UNUSED; return 0; } // uid/gid/euid/egid: root
long sys_setid(SYSCALL_ARGS) { SYSCALL_UNUSED; return 0; }

// ---- process groups and sessions ----

long sys_getpgid(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a0 == 0)
        return proc::current()->pgid;
    LockGuard guard(proc::table_lock());
    Process* p = proc::find(static_cast<int>(a0));
    return p ? p->pgid : -err::SRCH;
}

long sys_getpgrp(SYSCALL_ARGS) { SYSCALL_UNUSED; return proc::current()->pgid; }

long sys_setpgid(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Process* me = proc::current();
    int pid = a0 ? static_cast<int>(a0) : me->pid;
    int pgid = a1 ? static_cast<int>(a1) : pid;
    if (pgid < 0)
        return -err::INVAL;
    LockGuard guard(proc::table_lock());
    Process* p = proc::find(pid);
    if (!p || (p != me && p->parent != me))
        return -err::SRCH;
    if (p->sid != me->sid)
        return -err::PERM;
    p->pgid = pgid;
    return 0;
}

long sys_getsid(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a0 == 0)
        return proc::current()->sid;
    LockGuard guard(proc::table_lock());
    Process* p = proc::find(static_cast<int>(a0));
    return p ? p->sid : -err::SRCH;
}

long sys_setsid(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Process* me = proc::current();
    if (me->pgid == me->pid)
        return -err::PERM; // already a group leader
    me->sid = me->pgid = me->pid;
    return me->sid;
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
        signal::send_to_group(pid == 0 ? me->pgid : -pid, sig);
        return 0;
    }
    if (pid == -1) { // everyone but init and ourselves
        proc::for_each([](Process* p, void* arg) {
            int s = static_cast<int>(reinterpret_cast<uptr>(arg));
            if (p != proc::current() && p->pid != 1 && s)
                signal::send(p, s);
        }, reinterpret_cast<void*>(static_cast<uptr>(sig)));
        return 0;
    }
    LockGuard guard(proc::table_lock());
    Process* p = proc::find(pid);
    if (!p || p->state == proc::State::Zombie)
        return -err::SRCH;
    if (sig)
        signal::send(p, sig);
    return 0;
}

long sys_tgkill(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    LockGuard guard(proc::table_lock());
    sched::Thread* t = proc::find_thread(static_cast<int>(a1));
    if (!t || !t->process || t->process->pid != static_cast<int>(a0))
        return -err::SRCH;
    if (a2)
        signal::send_thread(t, static_cast<int>(a2));
    return 0;
}

long sys_tkill(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    LockGuard guard(proc::table_lock());
    sched::Thread* t = proc::find_thread(static_cast<int>(a0));
    if (!t)
        return -err::SRCH;
    if (a1)
        signal::send_thread(t, static_cast<int>(a1));
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
    case ARCH_SET_FS:
        if (a1 >= mm::USER_END)
            return -err::PERM;
        t->fs_base = a1;
        arch::wrmsr(MSR_FS_BASE, a1);
        return 0;
    case ARCH_SET_GS:
        if (a1 >= mm::USER_END)
            return -err::PERM;
        t->gs_base = a1;
        arch::wrmsr(MSR_KERNEL_GS_BASE, a1); // swapped in on return to user
        return 0;
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
    return sched::current()->user_tid;
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
    snprintf(u.nodename, 65, "syluxium");
    snprintf(u.release, 65, "6.12.0-syluxium");
    snprintf(u.version, 65, "#1 SMP Syluxium " __DATE__);
    snprintf(u.machine, 65, "x86_64");
    snprintf(u.domainname, 65, "(none)");
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

long sys_sigaltstack(SYSCALL_ARGS) { SYSCALL_UNUSED; return 0; }

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

void register_process() {
    set(13, sys_rt_sigaction, "rt_sigaction");
    set(14, sys_rt_sigprocmask, "rt_sigprocmask");
    set(24, sys_sched_yield, "sched_yield");
    set(39, sys_getpid, "getpid");
    set(60, sys_exit, "exit");
    set(63, sys_uname, "uname");
    set(97, sys_getrlimit, "getrlimit");
    set(99, sys_sysinfo, "sysinfo");
    set(103, sys_syslog, "syslog");
    set(102, sys_getid, "getuid");
    set(104, sys_getid, "getgid");
    set(105, sys_setid, "setuid");
    set(122, sys_getid, "setfsuid"); // returns the previous fsuid: always root
    set(123, sys_getid, "setfsgid");
    set(106, sys_setid, "setgid");
    set(107, sys_getid, "geteuid");
    set(108, sys_getid, "getegid");
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
    set(200, sys_tkill, "tkill");
    set(234, sys_tgkill, "tgkill");
    set(435, sys_quiet_nosys, "clone3");
    set(131, sys_sigaltstack, "sigaltstack");
    set(158, sys_arch_prctl, "arch_prctl");
    set(186, sys_gettid, "gettid");
    set(202, sys_futex, "futex");
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
