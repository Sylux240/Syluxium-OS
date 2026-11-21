// 32-bit (ia32) system calls: the i386 Linux ABI on top of the x86_64 handlers.
//
// A 32-bit program enters with `int $0x80` (or SYSCALL from compatibility
// mode on AMD), with i386 syscall numbers and arguments in ebx, ecx, edx,
// esi, edi, ebp. Most calls take the same arguments as on x86_64 - ints,
// pointers to buffers, strings, structs that have no pointer- or long-sized
// fields - and simply go to the native handler with their arguments zero-
// extended. The rest need translating, like Linux's compat layer does:
//
//  - structs with longs/pointers/time_t (stat64, statfs, timespec, timeval,
//    iovec, msghdr, sigaction, rlimit, sysinfo, itimerval, ...),
//  - 64-bit values split over two registers (pread64, _llseek, truncate64),
//  - different argument orders or multiplexers (clone, socketcall, mmap2),
//  - the 32-bit-only calls (set_thread_area, sigreturn, the 16-bit uid calls).
//
// Translation strategy: rather than duplicating every native handler, the
// compat wrapper converts the 32-bit struct into the 64-bit layout in a small
// scratch area below the caller's user stack pointer (memory a 32-bit
// program never relies on - signal frames are built there too), calls the
// native handler on that, and converts the result back. This is the same
// trick Linux used for years (compat_alloc_user_space). Calls that a 32-bit
// glibc never needs, or that Syluxium doesn't implement natively either
// (SysV IPC, epoll, inotify, ...), return -ENOSYS.

#include <arch/x86_64/compat_segments.hpp>
#include <core/log.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <proc/compat.hpp>
#include <proc/process.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>
#include <sched/sched.hpp>
#include <syscall/syscall.hpp>
#include <time/clock.hpp>

extern "C" void int80_entry(); // compat_entry.S

namespace syscall {

namespace {

constexpr u32 MAX_COMPAT = 450;

struct Entry {
    Handler handler;
    const char* name;
};

Entry g_table32[MAX_COMPAT];
bool g_warned32[MAX_COMPAT];

using interrupts::Frame;

// x86_64 numbers of the native handlers the wrappers call.
namespace nr64 {
constexpr u32 read = 0, write = 1, stat = 4, fstat = 5, lstat = 6, lseek = 8, mmap = 9, pread64 = 17,
              pwrite64 = 18, select = 23, nanosleep = 35, getitimer = 36, setitimer = 38, sendfile = 40,
              socket = 41, connect = 42, accept = 43, sendto = 44, recvfrom = 45, sendmsg = 46, recvmsg = 47,
              shutdown = 48, bind = 49, listen = 50, getsockname = 51, getpeername = 52, socketpair = 53,
              setsockopt = 54, getsockopt = 55, wait4 = 61, truncate = 76, ftruncate = 77, gettimeofday = 96,
              getrlimit = 97, sysinfo = 99, statfs = 137, fstatfs = 138, umount2 = 166, time = 201,
              clock_gettime = 228, clock_getres = 229, clock_nanosleep = 230, utimes = 235, newfstatat = 262,
              pselect6 = 270, ppoll = 271, utimensat = 280, accept4 = 288, futex = 202;
} // namespace nr64

long call64(u32 nr, u64 a0 = 0, u64 a1 = 0, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0, u64 a5 = 0,
            Frame* frame = nullptr) {
    Handler h = native(nr);
    return h ? h(a0, a1, a2, a3, a4, a5, frame) : -err::NOSYS;
}

// Scratch space for translated structs, carved out below the user's stack
// pointer (see the comment at the top). 256 bytes are skipped first so even
// code that (wrongly) keeps data just below %esp isn't disturbed.
class Scratch {
public:
    explicit Scratch(const Frame* f) : m_top(((f->rsp & 0xFFFFFFFF) - 256) & ~u64(15)) {}
    u64 alloc(usize n) {
        m_top = (m_top - n) & ~u64(15);
        return m_top;
    }

private:
    u64 m_top;
};

template <typename T>
T* uptr(u64 addr) { return reinterpret_cast<T*>(addr); }

i64 sext(u64 v) { return static_cast<i64>(static_cast<i32>(v)); } // a 32-bit signed argument
u64 pair64(u64 lo, u64 hi) { return (hi << 32) | (lo & 0xFFFFFFFF); }

// ---- time types ----

struct Timespec32 { i32 sec, nsec; };
struct Timeval32 { i32 sec, usec; };
struct Timespec64 { i64 sec, nsec; };
struct Timeval64 { i64 sec, usec; };

// Copy a user timespec32 (array of n) into scratch as native timespecs. 0 stays 0.
long timespec_in(Scratch& s, u64 user, u64* out, int n = 1) {
    *out = 0;
    if (!user)
        return 0;
    Timespec32 t32[2];
    Timespec64 t64[2];
    if (long r = uaccess::copy_from(t32, uptr<void>(user), n * sizeof(Timespec32)); r < 0)
        return r;
    for (int i = 0; i < n; i++)
        t64[i] = {t32[i].sec, t32[i].nsec};
    *out = s.alloc(n * sizeof(Timespec64));
    return uaccess::copy_to(uptr<void>(*out), t64, n * sizeof(Timespec64));
}

// The *_time64 calls take struct __kernel_timespec {s64 sec; s64 nsec}, but
// for 32-bit programs the upper half of tv_nsec is padding that glibc leaves
// uninitialized: like Linux, use only its low 32 bits.
long timespec64_in(Scratch& s, u64 user, u64* out, int n = 1) {
    *out = 0;
    if (!user)
        return 0;
    Timespec64 t[2];
    if (long r = uaccess::copy_from(t, uptr<void>(user), n * sizeof(Timespec64)); r < 0)
        return r;
    for (int i = 0; i < n; i++)
        t[i].nsec = static_cast<i32>(t[i].nsec);
    *out = s.alloc(n * sizeof(Timespec64));
    return uaccess::copy_to(uptr<void>(*out), t, n * sizeof(Timespec64));
}

long timeval_in(Scratch& s, u64 user, u64* out, int n = 1) {
    *out = 0;
    if (!user)
        return 0;
    Timeval32 t32[2];
    Timeval64 t64[2];
    if (long r = uaccess::copy_from(t32, uptr<void>(user), n * sizeof(Timeval32)); r < 0)
        return r;
    for (int i = 0; i < n; i++)
        t64[i] = {t32[i].sec, t32[i].usec};
    *out = s.alloc(n * sizeof(Timeval64));
    return uaccess::copy_to(uptr<void>(*out), t64, n * sizeof(Timeval64));
}

long timespec_out(u64 native, u64 user) {
    Timespec64 t;
    if (long r = uaccess::get(&t, uptr<Timespec64>(native)); r < 0)
        return r;
    return uaccess::put(uptr<Timespec32>(user), Timespec32{static_cast<i32>(t.sec), static_cast<i32>(t.nsec)});
}

// ---- process ----

long c_waitpid(SYSCALL_ARGS) { SYSCALL_UNUSED; return call64(nr64::wait4, a0, a1, a2, 0); }

long c_wait4(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    long r = call64(nr64::wait4, a0, a1, a2, 0);
    if (r > 0 && a3) {
        u8 zeros[72] = {}; // struct compat_rusage: no accounting yet
        uaccess::copy_to(uptr<void>(a3), zeros, sizeof(zeros));
    }
    return r;
}

// argv/envp are arrays of 32-bit pointers.
class StringVector32 {
public:
    ~StringVector32() { kfree(m_block); }

    long copy_from_user(u64 user_array) {
        constexpr usize MAX_STRINGS = 4096, MAX_BYTES = 2 * MiB;
        m_block = static_cast<u8*>(kmalloc(MAX_BYTES));
        if (!m_block)
            return -err::NOMEM;
        auto** ptrs = reinterpret_cast<char**>(m_block);
        char* strings = reinterpret_cast<char*>(m_block + (MAX_STRINGS + 1) * sizeof(char*));
        char* end = reinterpret_cast<char*>(m_block + MAX_BYTES);
        usize n = 0;
        for (; user_array; n++) {
            if (n >= MAX_STRINGS)
                return -err::TOOBIG;
            u32 ptr;
            if (long r = uaccess::get(&ptr, uptr<const u32>(user_array) + n); r < 0)
                return r;
            if (!ptr)
                break;
            long len = uaccess::string_from(uptr<const char>(ptr), strings, end - strings);
            if (len < 0)
                return len == -err::NAMETOOLONG ? -err::TOOBIG : len;
            ptrs[n] = strings;
            strings += len + 1;
        }
        ptrs[n] = nullptr;
        return 0;
    }

    const char* const* get() const { return reinterpret_cast<const char* const*>(m_block); }

private:
    u8* m_block = nullptr;
};

long c_execve(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    char* path = static_cast<char*>(kmalloc(4096));
    if (!path)
        return -err::NOMEM;
    long r = uaccess::string_from(uptr<const char>(a0), path, 4096);
    StringVector32 argv, envp;
    if (r >= 0)
        r = argv.copy_from_user(a1);
    if (r >= 0)
        r = envp.copy_from_user(a2);
    if (r >= 0)
        r = proc::do_execve(frame, path, argv.get(), envp.get());
    kfree(path);
    return r;
}

// i386 clone(flags, newsp, parent_tid, tls, child_tid) - tls and child_tid
// swapped relative to x86_64, and tls points at a struct user_desc.
long c_clone(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    constexpr u64 CLONE_SETTLS = 0x80000;
    if (a0 & CLONE_SETTLS) {
        compat::UserDesc d;
        if (uaccess::get(&d, uptr<const compat::UserDesc>(a3)) < 0)
            return -err::FAULT; // fail before anything is created
    }
    return proc::do_clone(frame, a0, a1, a2, a4, a3);
}

long c_set_thread_area(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    compat::UserDesc d;
    if (long r = uaccess::get(&d, uptr<compat::UserDesc>(a0)); r < 0)
        return r;
    sched::Thread* t = sched::current();
    if (long r = compat::set_tls(t, &d); r < 0)
        return r;
    compat_segments::install_current(t);
    return uaccess::put(&uptr<compat::UserDesc>(a0)->entry_number, d.entry_number);
}

long c_get_thread_area(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    compat::UserDesc d;
    if (long r = uaccess::get(&d, uptr<compat::UserDesc>(a0)); r < 0)
        return r;
    if (long r = compat::get_tls(sched::current(), &d); r < 0)
        return r;
    return uaccess::put(uptr<compat::UserDesc>(a0), d);
}

// i386 stack_t: { u32 ss_sp; int ss_flags; u32 ss_size; }
long c_sigaltstack(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    struct StackT32 {
        u32 ss_sp;
        i32 ss_flags;
        u32 ss_size;
    };
    signal::AltStack ns, os;
    if (a0) {
        StackT32 u;
        if (uaccess::get(&u, uptr<const StackT32>(a0)) < 0)
            return -err::FAULT;
        ns = signal::AltStack{u.ss_sp, static_cast<u32>(u.ss_flags), u.ss_size};
    }
    if (long r = signal::sigaltstack(a0 ? &ns : nullptr, &os, frame->rsp & 0xFFFFFFFF); r < 0)
        return r;
    if (a1)
        return uaccess::put(uptr<StackT32>(a1), StackT32{static_cast<u32>(os.sp), static_cast<i32>(os.flags),
                                                         static_cast<u32>(os.size)});
    return 0;
}

long c_sigreturn(SYSCALL_ARGS) { SYSCALL_UNUSED; return compat::sigreturn(frame, false); }
long c_rt_sigreturn(SYSCALL_ARGS) { SYSCALL_UNUSED; return compat::sigreturn(frame, true); }

// ---- signals ----

struct SigAction32 {
    u32 handler, flags, restorer;
    u32 mask[2];
};

struct OldSigAction32 {
    u32 handler, mask, flags, restorer;
};

long c_rt_sigaction(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    int sig = static_cast<int>(a0);
    if (sig < 1 || sig > proc::MAX_SIGNALS || (a3 && a3 != 8))
        return -err::INVAL;
    proc::Process* p = proc::current();
    SigAction32 nact;
    if (a1 && uaccess::get(&nact, uptr<SigAction32>(a1)) < 0)
        return -err::FAULT;
    if (a2) {
        const proc::SigAction& o = p->sigactions[sig];
        SigAction32 oact{static_cast<u32>(o.handler), static_cast<u32>(o.flags), static_cast<u32>(o.restorer),
                         {static_cast<u32>(o.mask), static_cast<u32>(o.mask >> 32)}};
        if (long r = uaccess::put(uptr<SigAction32>(a2), oact); r < 0)
            return r;
    }
    if (a1) {
        if (sig == signal::SIGKILL || sig == signal::SIGSTOP)
            return -err::INVAL;
        p->sigactions[sig] = proc::SigAction{nact.handler, nact.flags, nact.restorer,
                                             nact.mask[0] | (static_cast<u64>(nact.mask[1]) << 32)};
    }
    return 0;
}

long c_sigaction(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    int sig = static_cast<int>(a0);
    if (sig < 1 || sig > proc::MAX_SIGNALS)
        return -err::INVAL;
    proc::Process* p = proc::current();
    OldSigAction32 nact;
    if (a1 && uaccess::get(&nact, uptr<OldSigAction32>(a1)) < 0)
        return -err::FAULT;
    if (a2) {
        const proc::SigAction& o = p->sigactions[sig];
        OldSigAction32 oact{static_cast<u32>(o.handler), static_cast<u32>(o.mask), static_cast<u32>(o.flags),
                            static_cast<u32>(o.restorer)};
        if (long r = uaccess::put(uptr<OldSigAction32>(a2), oact); r < 0)
            return r;
    }
    if (a1) {
        if (sig == signal::SIGKILL || sig == signal::SIGSTOP)
            return -err::INVAL;
        p->sigactions[sig] = proc::SigAction{nact.handler, nact.flags, nact.restorer, nact.mask};
    }
    return 0;
}

// sigprocmask with the old 32-bit sigset (signals 1-32 only).
long c_sigprocmask(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    sched::Thread* t = sched::current();
    if (a2 && uaccess::put(uptr<u32>(a2), static_cast<u32>(t->sigmask)) < 0)
        return -err::FAULT;
    if (!a1)
        return 0;
    u32 set;
    if (uaccess::get(&set, uptr<u32>(a1)) < 0)
        return -err::FAULT;
    constexpr u64 UNBLOCKABLE = (1ull << (signal::SIGKILL - 1)) | (1ull << (signal::SIGSTOP - 1));
    switch (static_cast<int>(a0)) {
    case 0: t->sigmask |= set; break;                                       // SIG_BLOCK
    case 1: t->sigmask &= ~static_cast<u64>(set); break;                    // SIG_UNBLOCK
    case 2: t->sigmask = (t->sigmask & ~0xFFFFFFFFull) | set; break;        // SIG_SETMASK
    default: return -err::INVAL;
    }
    t->sigmask &= ~UNBLOCKABLE;
    return 0;
}

// ---- memory ----

long c_mmap2(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    return call64(nr64::mmap, a0, a1, a2, a3, static_cast<u64>(sext(a4)), a5 * mm::PAGE_SIZE);
}

// Old mmap(struct mmap_arg_struct*): all six arguments in memory, byte offset.
long c_old_mmap(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    u32 args[6];
    if (long r = uaccess::copy_from(args, uptr<void>(a0), sizeof(args)); r < 0)
        return r;
    if (args[5] & (mm::PAGE_SIZE - 1))
        return -err::INVAL;
    return call64(nr64::mmap, args[0], args[1], args[2], args[3], static_cast<u64>(sext(args[4])), args[5]);
}

// ---- files ----

long c_lseek(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    long r = call64(nr64::lseek, a0, static_cast<u64>(sext(a1)), a2);
    return r > 0x7FFFFFFF ? -err::OVERFLOW : r;
}

// _llseek(fd, offset_high, offset_low, loff_t* result, whence)
long c_llseek(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    long r = call64(nr64::lseek, a0, pair64(a2, a1), a4);
    if (r < 0)
        return r;
    i64 pos = r;
    return uaccess::put(uptr<i64>(a3), pos);
}

long c_pread64(SYSCALL_ARGS) { SYSCALL_UNUSED; return call64(nr64::pread64, a0, a1, a2, pair64(a3, a4)); }
long c_pwrite64(SYSCALL_ARGS) { SYSCALL_UNUSED; return call64(nr64::pwrite64, a0, a1, a2, pair64(a3, a4)); }
long c_truncate64(SYSCALL_ARGS) { SYSCALL_UNUSED; return call64(nr64::truncate, a0, pair64(a1, a2)); }
long c_ftruncate64(SYSCALL_ARGS) { SYSCALL_UNUSED; return call64(nr64::ftruncate, a0, pair64(a1, a2)); }
long c_truncate(SYSCALL_ARGS) { SYSCALL_UNUSED; return call64(nr64::truncate, a0, static_cast<u64>(sext(a1))); }
long c_ftruncate(SYSCALL_ARGS) { SYSCALL_UNUSED; return call64(nr64::ftruncate, a0, static_cast<u64>(sext(a1))); }
long c_umount(SYSCALL_ARGS) { SYSCALL_UNUSED; return call64(nr64::umount2, a0, 0); }
long c_zero(SYSCALL_ARGS) { SYSCALL_UNUSED; return 0; } // advice-only calls (fadvise64, readahead)

struct IoVec32 { u32 base, len; };

// readv/writev as a sequence of plain reads/writes (what the native ones do too).
long vectored32(u64 fd, u64 user_iov, u64 count, bool write) {
    if (count > 1024)
        return -err::INVAL;
    long total = 0;
    for (u64 i = 0; i < count; i++) {
        IoVec32 v;
        if (long r = uaccess::get(&v, uptr<IoVec32>(user_iov) + i); r < 0)
            return total ? total : r;
        if (v.len == 0)
            continue;
        long n = call64(write ? nr64::write : nr64::read, fd, v.base, v.len);
        if (n < 0)
            return total ? total : n;
        total += n;
        if (static_cast<u64>(n) < v.len)
            break; // short transfer
    }
    return total;
}

long c_readv(SYSCALL_ARGS) { SYSCALL_UNUSED; return vectored32(a0, a1, a2, false); }
long c_writev(SYSCALL_ARGS) { SYSCALL_UNUSED; return vectored32(a0, a1, a2, true); }

// sendfile with a 32-bit off_t*.
long c_sendfile(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (!a2)
        return call64(nr64::sendfile, a0, a1, 0, a3);
    i32 off32;
    if (uaccess::get(&off32, uptr<i32>(a2)) < 0)
        return -err::FAULT;
    Scratch s(frame);
    u64 off = s.alloc(8);
    i64 off64 = off32;
    if (long r = uaccess::put(uptr<i64>(off), off64); r < 0)
        return r;
    long r = call64(nr64::sendfile, a0, a1, off, a3);
    if (uaccess::get(&off64, uptr<i64>(off)) == 0)
        uaccess::put(uptr<i32>(a2), static_cast<i32>(off64));
    return r;
}

// ---- stat family ----

struct Stat64Native { // struct stat (x86_64), as the native handlers write it
    u64 st_dev, st_ino, st_nlink;
    u32 st_mode, st_uid, st_gid, pad0;
    u64 st_rdev;
    i64 st_size, st_blksize, st_blocks;
    i64 st_atime_sec, st_atime_nsec, st_mtime_sec, st_mtime_nsec, st_ctime_sec, st_ctime_nsec;
    i64 unused[3];
};
static_assert(sizeof(Stat64Native) == 144);

// struct stat64 (i386): 4-byte aligned 64-bit fields.
struct __attribute__((packed)) Stat64_32 {
    u64 st_dev;
    u8 pad0[4];
    u32 st_ino32;
    u32 st_mode;
    u32 st_nlink;
    u32 st_uid;
    u32 st_gid;
    u64 st_rdev;
    u8 pad3[4];
    i64 st_size;
    u32 st_blksize;
    u64 st_blocks;
    u32 st_atime, st_atime_nsec;
    u32 st_mtime, st_mtime_nsec;
    u32 st_ctime, st_ctime_nsec;
    u64 st_ino;
};
static_assert(sizeof(Stat64_32) == 96);

// The original struct stat (i386), still used by old binaries.
struct OldStat32 {
    u32 st_dev, st_ino;
    u16 st_mode, st_nlink, st_uid, st_gid;
    u32 st_rdev, st_size, st_blksize, st_blocks;
    u32 st_atime, st_atime_nsec, st_mtime, st_mtime_nsec, st_ctime, st_ctime_nsec;
    u32 unused4, unused5;
};
static_assert(sizeof(OldStat32) == 64);

// Run a native stat-style call that writes a Stat64Native at the pointer in
// argument slot `slot`, and fetch the result.
long native_stat(Frame* frame, u32 nr, u64 a0, u64 a1, u64 a2, u64 a3, int slot, Stat64Native* out) {
    Scratch s(frame);
    u64 buf = s.alloc(sizeof(Stat64Native));
    u64 args[4] = {a0, a1, a2, a3};
    args[slot] = buf;
    if (long r = call64(nr, args[0], args[1], args[2], args[3]); r < 0)
        return r;
    return uaccess::get(out, uptr<Stat64Native>(buf));
}

long put_stat64(const Stat64Native& n, u64 user) {
    Stat64_32 st{};
    st.st_dev = n.st_dev;
    st.st_ino32 = static_cast<u32>(n.st_ino);
    st.st_ino = n.st_ino;
    st.st_mode = n.st_mode;
    st.st_nlink = static_cast<u32>(n.st_nlink);
    st.st_uid = n.st_uid;
    st.st_gid = n.st_gid;
    st.st_rdev = n.st_rdev;
    st.st_size = n.st_size;
    st.st_blksize = static_cast<u32>(n.st_blksize);
    st.st_blocks = static_cast<u64>(n.st_blocks);
    st.st_atime = static_cast<u32>(n.st_atime_sec); st.st_atime_nsec = static_cast<u32>(n.st_atime_nsec);
    st.st_mtime = static_cast<u32>(n.st_mtime_sec); st.st_mtime_nsec = static_cast<u32>(n.st_mtime_nsec);
    st.st_ctime = static_cast<u32>(n.st_ctime_sec); st.st_ctime_nsec = static_cast<u32>(n.st_ctime_nsec);
    return uaccess::put(uptr<Stat64_32>(user), st);
}

long put_old_stat(const Stat64Native& n, u64 user) {
    if (n.st_ino > 0xFFFFFFFF || n.st_size > 0x7FFFFFFF || n.st_nlink > 0xFFFF)
        return -err::OVERFLOW;
    OldStat32 st{};
    st.st_dev = static_cast<u32>(n.st_dev);
    st.st_ino = static_cast<u32>(n.st_ino);
    st.st_mode = static_cast<u16>(n.st_mode);
    st.st_nlink = static_cast<u16>(n.st_nlink);
    st.st_uid = static_cast<u16>(n.st_uid);
    st.st_gid = static_cast<u16>(n.st_gid);
    st.st_rdev = static_cast<u32>(n.st_rdev);
    st.st_size = static_cast<u32>(n.st_size);
    st.st_blksize = static_cast<u32>(n.st_blksize);
    st.st_blocks = static_cast<u32>(n.st_blocks);
    st.st_atime = static_cast<u32>(n.st_atime_sec); st.st_atime_nsec = static_cast<u32>(n.st_atime_nsec);
    st.st_mtime = static_cast<u32>(n.st_mtime_sec); st.st_mtime_nsec = static_cast<u32>(n.st_mtime_nsec);
    st.st_ctime = static_cast<u32>(n.st_ctime_sec); st.st_ctime_nsec = static_cast<u32>(n.st_ctime_nsec);
    return uaccess::put(uptr<OldStat32>(user), st);
}

#define STAT_WRAPPER(name, nr, slot, put, ...)                                     \
    long name(SYSCALL_ARGS) {                                                      \
        SYSCALL_UNUSED;                                                            \
        Stat64Native n;                                                            \
        if (long r = native_stat(frame, nr, __VA_ARGS__, slot, &n); r < 0)         \
            return r;                                                              \
        return put(n, slot == 1 ? a1 : a2);                                        \
    }

STAT_WRAPPER(c_stat64, nr64::stat, 1, put_stat64, a0, 0, 0, 0)
STAT_WRAPPER(c_lstat64, nr64::lstat, 1, put_stat64, a0, 0, 0, 0)
STAT_WRAPPER(c_fstat64, nr64::fstat, 1, put_stat64, a0, 0, 0, 0)
STAT_WRAPPER(c_fstatat64, nr64::newfstatat, 2, put_stat64, a0, a1, 0, a3)
STAT_WRAPPER(c_stat, nr64::stat, 1, put_old_stat, a0, 0, 0, 0)
STAT_WRAPPER(c_lstat, nr64::lstat, 1, put_old_stat, a0, 0, 0, 0)
STAT_WRAPPER(c_fstat, nr64::fstat, 1, put_old_stat, a0, 0, 0, 0)
#undef STAT_WRAPPER

struct StatfsNative {
    i64 f_type, f_bsize;
    u64 f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    i32 f_fsid[2];
    i64 f_namelen, f_frsize, f_flags;
    i64 f_spare[4];
};
static_assert(sizeof(StatfsNative) == 120);

struct Statfs32 {
    u32 f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    i32 f_fsid[2];
    u32 f_namelen, f_frsize, f_flags;
    u32 f_spare[4];
};
static_assert(sizeof(Statfs32) == 64);

struct __attribute__((packed)) Statfs64_32 {
    u32 f_type, f_bsize;
    u64 f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    i32 f_fsid[2];
    u32 f_namelen, f_frsize, f_flags;
    u32 f_spare[4];
};
static_assert(sizeof(Statfs64_32) == 84);

long native_statfs(Frame* frame, u32 nr, u64 a0, StatfsNative* out) {
    Scratch s(frame);
    u64 buf = s.alloc(sizeof(StatfsNative));
    if (long r = call64(nr, a0, buf); r < 0)
        return r;
    return uaccess::get(out, uptr<StatfsNative>(buf));
}

long put_statfs(const StatfsNative& n, u64 user) {
    if ((n.f_blocks | n.f_bfree | n.f_bavail) > 0xFFFFFFFF ||
        (n.f_files != ~0ull && n.f_files > 0xFFFFFFFF) || (n.f_ffree != ~0ull && n.f_ffree > 0xFFFFFFFF))
        return -err::OVERFLOW;
    Statfs32 st{};
    st.f_type = static_cast<u32>(n.f_type);
    st.f_bsize = static_cast<u32>(n.f_bsize);
    st.f_blocks = static_cast<u32>(n.f_blocks);
    st.f_bfree = static_cast<u32>(n.f_bfree);
    st.f_bavail = static_cast<u32>(n.f_bavail);
    st.f_files = static_cast<u32>(n.f_files);
    st.f_ffree = static_cast<u32>(n.f_ffree);
    st.f_fsid[0] = n.f_fsid[0];
    st.f_fsid[1] = n.f_fsid[1];
    st.f_namelen = static_cast<u32>(n.f_namelen);
    st.f_frsize = static_cast<u32>(n.f_frsize);
    st.f_flags = static_cast<u32>(n.f_flags);
    return uaccess::put(uptr<Statfs32>(user), st);
}

long put_statfs64(const StatfsNative& n, u64 user) {
    Statfs64_32 st{};
    st.f_type = static_cast<u32>(n.f_type);
    st.f_bsize = static_cast<u32>(n.f_bsize);
    st.f_blocks = n.f_blocks;
    st.f_bfree = n.f_bfree;
    st.f_bavail = n.f_bavail;
    st.f_files = n.f_files;
    st.f_ffree = n.f_ffree;
    st.f_fsid[0] = n.f_fsid[0];
    st.f_fsid[1] = n.f_fsid[1];
    st.f_namelen = static_cast<u32>(n.f_namelen);
    st.f_frsize = static_cast<u32>(n.f_frsize);
    st.f_flags = static_cast<u32>(n.f_flags);
    return uaccess::put(uptr<Statfs64_32>(user), st);
}

long c_statfs(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    StatfsNative n;
    if (long r = native_statfs(frame, nr64::statfs, a0, &n); r < 0)
        return r;
    return put_statfs(n, a1);
}

long c_fstatfs(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    StatfsNative n;
    if (long r = native_statfs(frame, nr64::fstatfs, a0, &n); r < 0)
        return r;
    return put_statfs(n, a1);
}

// statfs64(path, size, buf) / fstatfs64(fd, size, buf)
long c_statfs64(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a1 != sizeof(Statfs64_32) && a1 != 88)
        return -err::INVAL;
    StatfsNative n;
    if (long r = native_statfs(frame, nr64::statfs, a0, &n); r < 0)
        return r;
    return put_statfs64(n, a2);
}

long c_fstatfs64(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a1 != sizeof(Statfs64_32) && a1 != 88)
        return -err::INVAL;
    StatfsNative n;
    if (long r = native_statfs(frame, nr64::fstatfs, a0, &n); r < 0)
        return r;
    return put_statfs64(n, a2);
}

// ---- time ----

long c_time(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    long t = call64(nr64::time, 0);
    if (a0 && uaccess::put(uptr<i32>(a0), static_cast<i32>(t)) < 0)
        return -err::FAULT;
    return t;
}

long c_gettimeofday(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a1) {
        u32 tz[2] = {0, 0}; // struct timezone: UTC, no DST
        if (uaccess::copy_to(uptr<void>(a1), tz, sizeof(tz)) < 0)
            return -err::FAULT;
    }
    if (!a0)
        return 0;
    Scratch s(frame);
    u64 tv = s.alloc(sizeof(Timeval64));
    if (long r = call64(nr64::gettimeofday, tv, 0); r < 0)
        return r;
    Timeval64 t;
    if (long r = uaccess::get(&t, uptr<Timeval64>(tv)); r < 0)
        return r;
    return uaccess::put(uptr<Timeval32>(a0), Timeval32{static_cast<i32>(t.sec), static_cast<i32>(t.usec)});
}

long c_clock_gettime(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Scratch s(frame);
    u64 ts = s.alloc(sizeof(Timespec64));
    if (long r = call64(nr64::clock_gettime, a0, ts); r < 0)
        return r;
    return timespec_out(ts, a1);
}

long c_clock_getres(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (!a1)
        return call64(nr64::clock_getres, a0, 0);
    Scratch s(frame);
    u64 ts = s.alloc(sizeof(Timespec64));
    if (long r = call64(nr64::clock_getres, a0, ts); r < 0)
        return r;
    return timespec_out(ts, a1);
}

long c_nanosleep(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Scratch s(frame);
    u64 req;
    if (long r = timespec_in(s, a0, &req); r < 0)
        return r;
    return call64(nr64::nanosleep, req, 0);
}

long c_clock_nanosleep(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Scratch s(frame);
    u64 req;
    if (long r = timespec_in(s, a2, &req); r < 0)
        return r;
    return call64(nr64::clock_nanosleep, a0, a1, req, 0);
}

// futex(uaddr, op, val, timeout_or_val2, uaddr2, val3): only the waits take a timespec.
long c_futex(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    constexpr u32 FUTEX_WAIT = 0, FUTEX_WAIT_BITSET = 9, FUTEX_CMD_MASK = 0x7F;
    u32 cmd = static_cast<u32>(a1) & FUTEX_CMD_MASK;
    u64 timeout = a3;
    Scratch s(frame);
    if ((cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) && a3) {
        if (long r = timespec_in(s, a3, &timeout); r < 0)
            return r;
    }
    return call64(nr64::futex, a0, a1, a2, timeout, a4, a5, frame);
}

// ---- *_time64 (64-bit time_t, native layout but see timespec64_in) ----

long c_clock_nanosleep64(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Scratch s(frame);
    u64 req;
    if (long r = timespec64_in(s, a2, &req); r < 0)
        return r;
    return call64(nr64::clock_nanosleep, a0, a1, req, 0);
}

long c_futex64(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    constexpr u32 FUTEX_WAIT = 0, FUTEX_WAIT_BITSET = 9, FUTEX_CMD_MASK = 0x7F;
    u32 cmd = static_cast<u32>(a1) & FUTEX_CMD_MASK;
    u64 timeout = a3;
    Scratch s(frame);
    if ((cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) && a3) {
        if (long r = timespec64_in(s, a3, &timeout); r < 0)
            return r;
    }
    return call64(nr64::futex, a0, a1, a2, timeout, a4, a5, frame);
}

long c_pselect6_64(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Scratch s(frame);
    u64 ts;
    if (long r = timespec64_in(s, a4, &ts); r < 0)
        return r;
    return call64(nr64::pselect6, a0, a1, a2, a3, ts, 0);
}

long c_ppoll64(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Scratch s(frame);
    u64 ts;
    if (long r = timespec64_in(s, a2, &ts); r < 0)
        return r;
    return call64(nr64::ppoll, a0, a1, ts, a3, a4);
}

long c_utimensat64(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Scratch s(frame);
    u64 ts;
    if (long r = timespec64_in(s, a2, &ts, 2); r < 0)
        return r;
    return call64(nr64::utimensat, a0, a1, ts, a3);
}

long c_select(SYSCALL_ARGS) { // _newselect(n, rd, wr, ex, timeval32*)
    SYSCALL_UNUSED;
    Scratch s(frame);
    u64 tv;
    if (long r = timeval_in(s, a4, &tv); r < 0)
        return r;
    return call64(nr64::select, a0, a1, a2, a3, tv);
}

long c_old_select(SYSCALL_ARGS) { // select(struct sel_arg_struct*)
    SYSCALL_UNUSED;
    u32 args[5];
    if (long r = uaccess::copy_from(args, uptr<void>(a0), sizeof(args)); r < 0)
        return r;
    return c_select(args[0], args[1], args[2], args[3], args[4], 0, frame);
}

long c_pselect6(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Scratch s(frame);
    u64 ts;
    if (long r = timespec_in(s, a4, &ts); r < 0)
        return r;
    return call64(nr64::pselect6, a0, a1, a2, a3, ts, 0); // sigmask: not applied natively either
}

long c_ppoll(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Scratch s(frame);
    u64 ts;
    if (long r = timespec_in(s, a2, &ts); r < 0)
        return r;
    return call64(nr64::ppoll, a0, a1, ts, a3, a4);
}

long c_utimes(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Scratch s(frame);
    u64 tv;
    if (long r = timeval_in(s, a1, &tv, 2); r < 0)
        return r;
    return call64(nr64::utimes, a0, tv);
}

long c_utime(SYSCALL_ARGS) { // utime(path, struct utimbuf32 {actime, modtime})
    SYSCALL_UNUSED;
    if (!a1)
        return call64(nr64::utimes, a0, 0);
    i32 times[2];
    if (long r = uaccess::copy_from(times, uptr<void>(a1), sizeof(times)); r < 0)
        return r;
    Scratch s(frame);
    u64 tv = s.alloc(2 * sizeof(Timeval64));
    Timeval64 t[2] = {{times[0], 0}, {times[1], 0}};
    if (long r = uaccess::copy_to(uptr<void>(tv), t, sizeof(t)); r < 0)
        return r;
    return call64(nr64::utimes, a0, tv);
}

long c_utimensat(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Scratch s(frame);
    u64 ts;
    if (long r = timespec_in(s, a2, &ts, 2); r < 0)
        return r;
    return call64(nr64::utimensat, a0, a1, ts, a3);
}

struct ITimerVal32 { i32 interval_sec, interval_usec, value_sec, value_usec; };
struct ITimerVal64 { i64 interval_sec, interval_usec, value_sec, value_usec; };

long itimer_out(u64 native, u64 user) {
    ITimerVal64 v;
    if (long r = uaccess::get(&v, uptr<ITimerVal64>(native)); r < 0)
        return r;
    return uaccess::put(uptr<ITimerVal32>(user),
                        ITimerVal32{static_cast<i32>(v.interval_sec), static_cast<i32>(v.interval_usec),
                                    static_cast<i32>(v.value_sec), static_cast<i32>(v.value_usec)});
}

long c_setitimer(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Scratch s(frame);
    u64 nv = 0, ov = 0;
    if (a1) {
        ITimerVal32 v;
        if (long r = uaccess::get(&v, uptr<ITimerVal32>(a1)); r < 0)
            return r;
        nv = s.alloc(sizeof(ITimerVal64));
        if (long r = uaccess::put(uptr<ITimerVal64>(nv),
                                  ITimerVal64{v.interval_sec, v.interval_usec, v.value_sec, v.value_usec});
            r < 0)
            return r;
    }
    if (a2)
        ov = s.alloc(sizeof(ITimerVal64));
    if (long r = call64(nr64::setitimer, a0, nv, ov); r < 0)
        return r;
    return a2 ? itimer_out(ov, a2) : 0;
}

long c_getitimer(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Scratch s(frame);
    u64 v = s.alloc(sizeof(ITimerVal64));
    if (long r = call64(nr64::getitimer, a0, v); r < 0)
        return r;
    return itimer_out(v, a1);
}

long c_times(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    if (a0) {
        u32 tms[4] = {}; // no per-process CPU accounting in clock ticks yet
        if (uaccess::copy_to(uptr<void>(a0), tms, sizeof(tms)) < 0)
            return -err::FAULT;
    }
    return static_cast<long>(static_cast<u32>(clock::now_ns() / 10'000'000)); // USER_HZ = 100
}

// ---- limits, ids, system info ----

long rlimit_out(Frame* frame, u64 res, u64 user, u32 infinity) {
    Scratch s(frame);
    u64 buf = s.alloc(16);
    if (long r = call64(nr64::getrlimit, res, buf); r < 0)
        return r;
    u64 lim[2];
    if (long r = uaccess::copy_from(lim, uptr<void>(buf), sizeof(lim)); r < 0)
        return r;
    u32 out[2] = {lim[0] > infinity ? infinity : static_cast<u32>(lim[0]),
                  lim[1] > infinity ? infinity : static_cast<u32>(lim[1])};
    return uaccess::copy_to(uptr<void>(user), out, sizeof(out));
}

long c_ugetrlimit(SYSCALL_ARGS) { SYSCALL_UNUSED; return rlimit_out(frame, a0, a1, 0xFFFFFFFF); }
long c_old_getrlimit(SYSCALL_ARGS) { SYSCALL_UNUSED; return rlimit_out(frame, a0, a1, 0x7FFFFFFF); }

long c_getresid(SYSCALL_ARGS) { // getresuid32/getresgid32: everyone is root
    SYSCALL_UNUSED;
    u32 zero = 0;
    if ((a0 && uaccess::put(uptr<u32>(a0), zero) < 0) || (a1 && uaccess::put(uptr<u32>(a1), zero) < 0) ||
        (a2 && uaccess::put(uptr<u32>(a2), zero) < 0))
        return -err::FAULT;
    return 0;
}

long c_getresid16(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    u16 zero = 0;
    if ((a0 && uaccess::put(uptr<u16>(a0), zero) < 0) || (a1 && uaccess::put(uptr<u16>(a1), zero) < 0) ||
        (a2 && uaccess::put(uptr<u16>(a2), zero) < 0))
        return -err::FAULT;
    return 0;
}

struct SysInfoNative {
    i64 uptime;
    u64 loads[3];
    u64 totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    u16 procs, pad;
    u64 totalhigh, freehigh;
    u32 mem_unit;
    char reserved[4];
};

struct SysInfo32 {
    i32 uptime;
    u32 loads[3];
    u32 totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    u16 procs, pad;
    u32 totalhigh, freehigh;
    u32 mem_unit;
    char reserved[8];
};
static_assert(sizeof(SysInfo32) == 64);

long c_sysinfo(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    Scratch s(frame);
    u64 buf = s.alloc(sizeof(SysInfoNative));
    if (long r = call64(nr64::sysinfo, buf); r < 0)
        return r;
    SysInfoNative n;
    if (long r = uaccess::get(&n, uptr<SysInfoNative>(buf)); r < 0)
        return r;
    // Byte counts above 4 GiB don't fit: switch to a bigger unit, as Linux does.
    u32 shift = 0;
    u64 unit = n.mem_unit ? n.mem_unit : 1;
    while ((n.totalram >> shift) > 0xFFFFFFFF || (n.totalswap >> shift) > 0xFFFFFFFF)
        shift++;
    SysInfo32 o{};
    o.uptime = static_cast<i32>(n.uptime);
    for (int i = 0; i < 3; i++)
        o.loads[i] = static_cast<u32>(n.loads[i]);
    o.totalram = static_cast<u32>(n.totalram >> shift);
    o.freeram = static_cast<u32>(n.freeram >> shift);
    o.sharedram = static_cast<u32>(n.sharedram >> shift);
    o.bufferram = static_cast<u32>(n.bufferram >> shift);
    o.totalswap = static_cast<u32>(n.totalswap >> shift);
    o.freeswap = static_cast<u32>(n.freeswap >> shift);
    o.procs = n.procs;
    o.mem_unit = static_cast<u32>(unit << shift);
    return uaccess::put(uptr<SysInfo32>(a0), o);
}

// ---- sockets ----

struct MsgHdr32 {
    u32 name, namelen, iov, iovlen, control, controllen;
    i32 flags;
};

struct MsgHdr64 {
    u64 name;
    u32 namelen, pad;
    u64 iov, iovlen, control, controllen;
    i32 flags, pad2;
};

struct IoVec64 { u64 base, len; };

constexpr u32 MAX_MSG_IOV = 64; // iovecs per message that fit the scratch area

// Translate a compat msghdr (and its iovec array) into scratch; returns the native address.
long msghdr_in(Scratch& s, u64 user, MsgHdr32* m32, u64* out) {
    if (long r = uaccess::get(m32, uptr<MsgHdr32>(user)); r < 0)
        return r;
    if (m32->iovlen > MAX_MSG_IOV)
        return -90; // EMSGSIZE
    IoVec64 iov[MAX_MSG_IOV];
    for (u32 i = 0; i < m32->iovlen; i++) {
        IoVec32 v;
        if (long r = uaccess::get(&v, uptr<IoVec32>(m32->iov) + i); r < 0)
            return r;
        iov[i] = {v.base, v.len};
    }
    u64 iov_addr = s.alloc(max<u32>(m32->iovlen, 1) * sizeof(IoVec64));
    if (long r = uaccess::copy_to(uptr<void>(iov_addr), iov, m32->iovlen * sizeof(IoVec64)); r < 0)
        return r;
    // Ancillary data isn't supported by the native socket layer: not passed on.
    MsgHdr64 m{m32->name, m32->namelen, 0, iov_addr, m32->iovlen, 0, 0, m32->flags, 0};
    *out = s.alloc(sizeof(MsgHdr64));
    return uaccess::put(uptr<MsgHdr64>(*out), m);
}

long sendmsg32(Frame* frame, u64 fd, u64 umsg, u64 flags) {
    Scratch s(frame);
    MsgHdr32 m32;
    u64 m;
    if (long r = msghdr_in(s, umsg, &m32, &m); r < 0)
        return r;
    return call64(nr64::sendmsg, fd, m, flags);
}

long recvmsg32(Frame* frame, u64 fd, u64 umsg, u64 flags) {
    Scratch s(frame);
    MsgHdr32 m32;
    u64 m;
    if (long r = msghdr_in(s, umsg, &m32, &m); r < 0)
        return r;
    long n = call64(nr64::recvmsg, fd, m, flags);
    if (n < 0)
        return n;
    MsgHdr64 out;
    if (uaccess::get(&out, uptr<MsgHdr64>(m)) == 0) {
        m32.namelen = out.namelen;
        m32.controllen = 0;
        m32.flags = out.flags;
        uaccess::put(uptr<MsgHdr32>(umsg), m32);
    }
    return n;
}

long c_sendmsg(SYSCALL_ARGS) { SYSCALL_UNUSED; return sendmsg32(frame, a0, a1, a2); }
long c_recvmsg(SYSCALL_ARGS) { SYSCALL_UNUSED; return recvmsg32(frame, a0, a1, a2); }

// struct compat_mmsghdr = compat msghdr (28 bytes) + u32 msg_len.
long mmsg32(Frame* frame, u64 fd, u64 vec, u64 vlen, u64 flags, bool send) {
    u32 count = static_cast<u32>(min<u64>(vlen, 1024));
    u32 done = 0;
    for (; done < count; done++) {
        u64 entry = vec + done * 32ull;
        // As natively: only the first receive may block.
        constexpr u64 MSG_DONTWAIT = 0x40;
        long r = send ? sendmsg32(frame, fd, entry, flags)
                      : recvmsg32(frame, fd, entry, done ? (flags | MSG_DONTWAIT) : flags);
        if (r < 0)
            return done ? done : r;
        uaccess::put(uptr<u32>(entry + 28), static_cast<u32>(r));
    }
    return done;
}

long c_sendmmsg(SYSCALL_ARGS) { SYSCALL_UNUSED; return mmsg32(frame, a0, a1, a2, a3, true); }
long c_recvmmsg(SYSCALL_ARGS) { SYSCALL_UNUSED; return mmsg32(frame, a0, a1, a2, a3, false); }

// socketcall(call, args*): every socket operation through one multiplexer
// (what i386 glibc used before the direct socket syscalls existed).
long c_socketcall(SYSCALL_ARGS) {
    SYSCALL_UNUSED;
    static constexpr u8 NARGS[21] = {0, 3, 3, 3, 2, 3, 3, 3, 4, 4, 4, 6, 6, 2, 5, 5, 3, 3, 4, 5, 4};
    if (a0 < 1 || a0 > 20)
        return -err::INVAL;
    u32 v[6] = {};
    if (long r = uaccess::copy_from(v, uptr<void>(a1), NARGS[a0] * sizeof(u32)); r < 0)
        return r;
    switch (a0) {
    case 1:  return call64(nr64::socket, v[0], v[1], v[2]);
    case 2:  return call64(nr64::bind, v[0], v[1], v[2]);
    case 3:  return call64(nr64::connect, v[0], v[1], v[2]);
    case 4:  return call64(nr64::listen, v[0], v[1]);
    case 5:  return call64(nr64::accept, v[0], v[1], v[2]);
    case 6:  return call64(nr64::getsockname, v[0], v[1], v[2]);
    case 7:  return call64(nr64::getpeername, v[0], v[1], v[2]);
    case 8:  return call64(nr64::socketpair, v[0], v[1], v[2], v[3]);
    case 9:  return call64(nr64::sendto, v[0], v[1], v[2], v[3], 0, 0);    // send
    case 10: return call64(nr64::recvfrom, v[0], v[1], v[2], v[3], 0, 0);  // recv
    case 11: return call64(nr64::sendto, v[0], v[1], v[2], v[3], v[4], v[5]);
    case 12: return call64(nr64::recvfrom, v[0], v[1], v[2], v[3], v[4], v[5]);
    case 13: return call64(nr64::shutdown, v[0], v[1]);
    case 14: return call64(nr64::setsockopt, v[0], v[1], v[2], v[3], v[4]);
    case 15: return call64(nr64::getsockopt, v[0], v[1], v[2], v[3], v[4]);
    case 16: return sendmsg32(frame, v[0], v[1], v[2]);
    case 17: return recvmsg32(frame, v[0], v[1], v[2]);
    case 18: return call64(nr64::accept4, v[0], v[1], v[2], v[3]);
    case 19: return mmsg32(frame, v[0], v[1], v[2], v[3], false);
    case 20: return mmsg32(frame, v[0], v[1], v[2], v[3], true);
    default: return -err::INVAL;
    }
}

// ---- misc ----

long c_arch_prctl(SYSCALL_ARGS) { SYSCALL_UNUSED; return -err::INVAL; } // only ARCH_*_CPUID exist for i386
long c_quiet_nosys(SYSCALL_ARGS) { SYSCALL_UNUSED; return -err::NOSYS; }

// ---- table ----

void set32(u32 nr, Handler h, const char* name) {
    if (nr < MAX_COMPAT)
        g_table32[nr] = Entry{h, name};
}

// Same arguments as the x86_64 call: use the native handler directly.
void map32(u32 nr, u32 native_nr, const char* name) {
    Handler h = native(native_nr);
    if (!h) {
        klog::warn("compat32: %s (x86_64 #%u) has no native handler", name, native_nr);
        return;
    }
    set32(nr, h, name);
}

struct Mapping {
    u16 nr32, nr64;
    const char* name;
};

// i386 number -> x86_64 number for calls whose arguments need no translation.
constexpr Mapping IDENTICAL[] = {
    {1, 60, "exit"}, {2, 57, "fork"}, {3, 0, "read"}, {4, 1, "write"}, {5, 2, "open"}, {6, 3, "close"},
    {8, 85, "creat"}, {10, 87, "unlink"}, {12, 80, "chdir"}, {133, 81, "fchdir"}, {383, 332, "statx"}, {341, 303, "name_to_handle_at"}, {15, 90, "chmod"}, {16, 94, "lchown16"},
    {20, 39, "getpid"}, {21, 165, "mount"}, {23, 105, "setuid16"}, {24, 102, "getuid16"}, {27, 37, "alarm"},
    {29, 34, "pause"}, {33, 21, "access"}, {36, 162, "sync"}, {37, 62, "kill"}, {38, 82, "rename"},
    {39, 83, "mkdir"}, {40, 84, "rmdir"}, {41, 32, "dup"}, {42, 22, "pipe"}, {45, 12, "brk"},
    {46, 106, "setgid16"}, {47, 104, "getgid16"}, {49, 107, "geteuid16"}, {50, 108, "getegid16"},
    {52, 166, "umount2"}, {54, 16, "ioctl"}, {55, 72, "fcntl"}, {57, 109, "setpgid"}, {60, 95, "umask"},
    {63, 33, "dup2"}, {64, 110, "getppid"}, {65, 111, "getpgrp"}, {66, 112, "setsid"}, {83, 88, "symlink"},
    {85, 89, "readlink"}, {91, 11, "munmap"}, {94, 91, "fchmod"}, {95, 93, "fchown16"}, {103, 103, "syslog"},
    {118, 74, "fsync"}, {122, 63, "uname"}, {125, 10, "mprotect"}, {132, 121, "getpgid"}, {147, 124, "getsid"},
    {148, 75, "fdatasync"}, {158, 24, "sched_yield"}, {168, 7, "poll"}, {172, 157, "prctl"},
    {175, 14, "rt_sigprocmask"}, {179, 130, "rt_sigsuspend"}, {182, 92, "chown16"}, {183, 79, "getcwd"},
{190, 58, "vfork"}, {198, 94, "lchown32"}, {199, 102, "getuid32"},
    {200, 104, "getgid32"}, {201, 107, "geteuid32"}, {202, 108, "getegid32"}, {203, 105, "setreuid32"},
    {204, 106, "setregid32"}, {207, 93, "fchown32"}, {208, 105, "setresuid32"}, {210, 106, "setresgid32"},
    {212, 92, "chown32"}, {213, 105, "setuid32"}, {214, 106, "setgid32"}, {215, 122, "setfsuid32"},
    {216, 123, "setfsgid32"}, {219, 28, "madvise"}, {220, 217, "getdents64"}, {221, 72, "fcntl64"},
    {224, 186, "gettid"}, {238, 200, "tkill"}, {239, 40, "sendfile64"}, {242, 204, "sched_getaffinity"},
    {252, 231, "exit_group"}, {258, 218, "set_tid_address"}, {270, 234, "tgkill"}, {295, 257, "openat"},
    {296, 258, "mkdirat"}, {298, 260, "fchownat"}, {301, 263, "unlinkat"}, {302, 264, "renameat"},
    {304, 266, "symlinkat"}, {305, 267, "readlinkat"}, {306, 268, "fchmodat"}, {307, 269, "faccessat"},
    {311, 273, "set_robust_list"}, {318, 309, "getcpu"}, {330, 292, "dup3"}, {331, 293, "pipe2"},
    {340, 302, "prlimit64"}, {136, 135, "personality"}, {154, 142, "sched_setparam"},
    {155, 143, "sched_getparam"}, {156, 144, "sched_setscheduler"}, {157, 145, "sched_getscheduler"},
    {159, 146, "sched_get_priority_max"}, {160, 147, "sched_get_priority_min"}, {178, 129, "rt_sigqueueinfo"},
    {335, 297, "rt_tgsigqueueinfo"}, {353, 316, "renameat2"},
    // Same argument layout as the native calls (Android's 32-bit userland uses them).
    {9, 86, "link"}, {14, 133, "mknod"}, {61, 161, "chroot"}, {74, 170, "sethostname"}, {88, 169, "reboot"},
    {96, 140, "getpriority"}, {97, 141, "setpriority"}, {121, 171, "setdomainname"}, {144, 26, "msync"},
    {150, 149, "mlock"}, {151, 150, "munlock"}, {152, 151, "mlockall"}, {153, 152, "munlockall"},
    {163, 25, "mremap"}, {184, 125, "capget"}, {185, 126, "capset"}, {217, 155, "pivot_root"},
    {254, 213, "epoll_create"}, {255, 233, "epoll_ctl"}, {256, 232, "epoll_wait"}, {319, 281, "epoll_pwait"},
    {289, 251, "ioprio_set"}, {290, 252, "ioprio_get"}, {291, 253, "inotify_init"},
    {292, 254, "inotify_add_watch"}, {293, 255, "inotify_rm_watch"}, {332, 294, "inotify_init1"},
    {297, 259, "mknodat"}, {310, 272, "unshare"}, {322, 283, "timerfd_create"}, {327, 289, "signalfd4"},
    {328, 290, "eventfd2"}, {329, 291, "epoll_create1"}, {344, 306, "syncfs"}, {346, 308, "setns"},
    {349, 312, "kcmp"}, {356, 319, "memfd_create"}, {375, 324, "membarrier"}, {355, 318, "getrandom"}, {359, 41, "socket"},
    {360, 53, "socketpair"}, {361, 49, "bind"}, {362, 42, "connect"}, {363, 50, "listen"},
    {364, 288, "accept4"}, {365, 55, "getsockopt"}, {366, 54, "setsockopt"}, {367, 51, "getsockname"},
    {368, 52, "getpeername"}, {369, 44, "sendto"}, {371, 45, "recvfrom"}, {373, 48, "shutdown"},
    // Output-only *_time64 calls: native layout (inputs are sanitized, see timespec64_in).
    {403, 228, "clock_gettime64"}, {406, 229, "clock_getres_time64"}, {439, 439, "faccessat2"},
};

} // namespace

void register_compat() {
    for (const Mapping& m : IDENTICAL)
        map32(m.nr32, m.nr64, m.name);

    set32(7, c_waitpid, "waitpid");
    set32(11, c_execve, "execve");
    set32(13, c_time, "time");
    set32(19, c_lseek, "lseek");
    set32(22, c_umount, "umount");
    set32(30, c_utime, "utime");
    set32(43, c_times, "times");
    set32(67, c_sigaction, "sigaction");
    set32(75, c_zero, "setrlimit"); // accepted, not enforced (as natively)
    set32(76, c_old_getrlimit, "getrlimit");
    set32(78, c_gettimeofday, "gettimeofday");
    set32(80, c_zero, "getgroups16");
    set32(81, c_zero, "setgroups16");
    set32(82, c_old_select, "select");
    set32(90, c_old_mmap, "mmap");
    set32(92, c_truncate, "truncate");
    set32(93, c_ftruncate, "ftruncate");
    set32(99, c_statfs, "statfs");
    set32(100, c_fstatfs, "fstatfs");
    set32(102, c_socketcall, "socketcall");
    set32(104, c_setitimer, "setitimer");
    set32(105, c_getitimer, "getitimer");
    set32(106, c_stat, "stat");
    set32(107, c_lstat, "lstat");
    set32(108, c_fstat, "fstat");
    set32(114, c_wait4, "wait4");
    set32(116, c_sysinfo, "sysinfo");
    set32(119, c_sigreturn, "sigreturn");
    set32(120, c_clone, "clone");
    set32(126, c_sigprocmask, "sigprocmask");
    set32(140, c_llseek, "_llseek");
    set32(142, c_select, "_newselect");
    set32(145, c_readv, "readv");
    set32(146, c_writev, "writev");
    set32(162, c_nanosleep, "nanosleep");
    set32(165, c_getresid16, "getresuid16");
    set32(171, c_getresid16, "getresgid16");
    set32(173, c_rt_sigreturn, "rt_sigreturn");
    set32(174, c_rt_sigaction, "rt_sigaction");
    set32(180, c_pread64, "pread64");
    set32(181, c_pwrite64, "pwrite64");
    set32(187, c_sendfile, "sendfile");
    set32(191, c_ugetrlimit, "ugetrlimit");
    set32(192, c_mmap2, "mmap2");
    set32(193, c_truncate64, "truncate64");
    set32(194, c_ftruncate64, "ftruncate64");
    set32(195, c_stat64, "stat64");
    set32(196, c_lstat64, "lstat64");
    set32(197, c_fstat64, "fstat64");
    set32(205, c_zero, "getgroups32");
    set32(206, c_zero, "setgroups32");
    set32(209, c_getresid, "getresuid32");
    set32(211, c_getresid, "getresgid32");
    set32(225, c_zero, "readahead");
    set32(240, c_futex, "futex");
    set32(243, c_set_thread_area, "set_thread_area");
    set32(186, c_sigaltstack, "sigaltstack");
    set32(244, c_get_thread_area, "get_thread_area");
    set32(250, c_zero, "fadvise64");
    set32(265, c_clock_gettime, "clock_gettime");
    set32(266, c_clock_getres, "clock_getres");
    set32(267, c_clock_nanosleep, "clock_nanosleep");
    set32(268, c_statfs64, "statfs64");
    set32(269, c_fstatfs64, "fstatfs64");
    set32(271, c_utimes, "utimes");
    set32(272, c_zero, "fadvise64_64");
    set32(300, c_fstatat64, "fstatat64");
    set32(308, c_pselect6, "pselect6");
    set32(309, c_ppoll, "ppoll");
    set32(320, c_utimensat, "utimensat");
    set32(337, c_recvmmsg, "recvmmsg");
    set32(345, c_sendmmsg, "sendmmsg");
    set32(370, c_sendmsg, "sendmsg");
    set32(372, c_recvmsg, "recvmsg");
    set32(407, c_clock_nanosleep64, "clock_nanosleep_time64");
    set32(412, c_utimensat64, "utimensat_time64");
    set32(413, c_pselect6_64, "pselect6_time64");
    set32(414, c_ppoll64, "ppoll_time64");
    set32(422, c_futex64, "futex_time64");
    set32(384, c_arch_prctl, "arch_prctl");
    // Optional features glibc probes for and copes without: no warning.
    set32(386, c_quiet_nosys, "rseq");
    set32(435, c_quiet_nosys, "clone3");

    interrupts::set_user_gate(interrupts::VEC_SYSCALL_INT80, int80_entry);
}

} // namespace syscall

namespace {

long compat_call(interrupts::Frame* frame, u64 nr, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    using namespace syscall;
    if (nr < MAX_COMPAT && g_table32[nr].handler)
        return g_table32[nr].handler(a0, a1, a2, a3, a4, a5, frame);
    if (nr < MAX_COMPAT && !g_warned32[nr]) {
        g_warned32[nr] = true;
        proc::Process* p = proc::current();
        klog::warn("%s[%d]: unimplemented 32-bit syscall %lu", p ? p->name : "?", p ? p->pid : 0, nr);
    }
    return -err::NOSYS;
}

void finish(interrupts::Frame* frame, long result) {
    frame->rax = static_cast<u64>(result);
    // Back to user mode: run pending signals' handlers / default actions.
    if (signal::pending(proc::current()))
        signal::deliver(frame);
}

constexpr u64 LOW32 = 0xFFFFFFFF;

} // namespace

// int $0x80. Only the low halves of the registers are the 32-bit program's
// (the upper halves are undefined after compatibility mode): zero-extend.
extern "C" void compat_syscall_dispatch(interrupts::Frame* frame) {
    u64 nr = frame->rax & LOW32;
    frame->error_code = nr; // original number, for SA_RESTART restarts
    long r = compat_call(frame, nr, frame->rbx & LOW32, frame->rcx & LOW32, frame->rdx & LOW32,
                         frame->rsi & LOW32, frame->rdi & LOW32, frame->rbp & LOW32);
    finish(frame, r);
}

// SYSCALL from compatibility mode: ECX holds the return address, so argument
// 2 comes in EBP and argument 6 from the user stack (Linux's convention).
extern "C" void compat_syscall32_dispatch(interrupts::Frame* frame) {
    u64 nr = frame->rax & LOW32;
    frame->error_code = nr;
    u32 arg6 = 0;
    uaccess::get(&arg6, reinterpret_cast<const u32*>(frame->rsp & LOW32));
    long r = compat_call(frame, nr, frame->rbx & LOW32, frame->rbp & LOW32, frame->rdx & LOW32,
                         frame->rsi & LOW32, frame->rdi & LOW32, arg6);
    finish(frame, r);
}
