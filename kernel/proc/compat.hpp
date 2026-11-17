#pragma once

#include <arch/x86_64/interrupts.hpp>
#include <proc/memory.hpp>
#include <proc/process.hpp>

// Process-level support for 32-bit (ia32) programs: the vsyscall page,
// 32-bit signal frames, and TLS descriptors (set_thread_area / clone).
// The 32-bit system call table itself lives in syscall/compat32.cpp.
namespace compat {

// The vsyscall page every 32-bit process gets at COMPAT_VSYSCALL. glibc
// finds __kernel_vsyscall through AT_SYSINFO and calls it for every system
// call; the sigreturn trampolines are used for handlers installed without
// SA_RESTORER (Linux keeps the same three entry points in its 32-bit vDSO).
constexpr uptr VSYSCALL_ENTRY        = proc::COMPAT_VSYSCALL;         // int $0x80; ret
constexpr uptr VSYSCALL_SIGRETURN    = proc::COMPAT_VSYSCALL + 0x10;  // popl %eax; movl $119,%eax; int $0x80
constexpr uptr VSYSCALL_RT_SIGRETURN = proc::COMPAT_VSYSCALL + 0x20;  // movl $173,%eax; int $0x80

// Map and fill the vsyscall page (read + execute) in a new address space.
long map_vsyscall_page(proc::Memory* mem);

// Register frame a 32-bit program starts with.
interrupts::Frame initial_frame(u64 entry, u64 stack_ptr);

// ---- Signals (Linux ia32 sigframe / rt_sigframe layouts) ----

// Build the handler frame on the user stack and point the registers at the
// handler. False if the frame can't be written (the process must die).
bool setup_signal_frame(interrupts::Frame* frame, int sig, const proc::SigAction& act, u64 blocked_mask);

// sigreturn (119) / rt_sigreturn (173): restore the interrupted context.
long sigreturn(interrupts::Frame* frame, bool rt);

// ---- TLS descriptors ----

// struct user_desc (set_thread_area, clone(CLONE_SETTLS) on i386).
struct UserDesc {
    u32 entry_number;
    u32 base_addr;
    u32 limit;
    u32 flags; // seg_32bit:1 contents:2 read_exec_only:1 limit_in_pages:1 seg_not_present:1 useable:1
};
static_assert(sizeof(UserDesc) == 16);

// Install `d` into one of t's TLS slots (entry_number -1: pick a free one
// and report it back through d->entry_number). 0 or -errno.
long set_tls(sched::Thread* t, UserDesc* d);
long get_tls(sched::Thread* t, UserDesc* d);

// clone()/fork() from a 32-bit thread: the child inherits the parent's TLS
// descriptors and live selectors; with CLONE_SETTLS `tls` points at a
// user_desc for the child. 0 or -errno.
long clone_tls(sched::Thread* child, sched::Thread* parent, bool settls, u64 tls);

} // namespace compat
