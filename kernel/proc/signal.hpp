#pragma once

#include <arch/x86_64/interrupts.hpp>
#include <lib/types.hpp>

namespace proc {
struct Process;
}
namespace sched {
struct Thread;
}

// POSIX signals with Linux numbering and Linux's x86_64 signal-frame layout,
// so glibc's handlers and sigreturn trampolines work unmodified.
namespace signal {

constexpr int SIGHUP = 1, SIGINT = 2, SIGQUIT = 3, SIGILL = 4, SIGTRAP = 5, SIGABRT = 6, SIGBUS = 7,
              SIGFPE = 8, SIGKILL = 9, SIGUSR1 = 10, SIGSEGV = 11, SIGUSR2 = 12, SIGPIPE = 13,
              SIGALRM = 14, SIGTERM = 15, SIGCHLD = 17, SIGCONT = 18, SIGSTOP = 19, SIGTSTP = 20,
              SIGTTIN = 21, SIGTTOU = 22, SIGURG = 23, SIGWINCH = 28;

// Queue a signal for a process and wake it if it's sleeping. Interrupt-safe.
// si_code values (who sent a signal; glibc's internal signals check them).
constexpr i32 SI_USER = 0, SI_KERNEL = 0x80, SI_TIMER = -2, SI_TKILL = -6;
void send(proc::Process* p, int sig);                 // from the kernel (SI_KERNEL)
void send(proc::Process* p, int sig, i32 code);       // kill(): SI_USER, with the sender's pid/uid
void send_to_group(int pgid, int sig);
void send_thread(sched::Thread* t, int sig); // to one specific thread, from the kernel
void send_thread(sched::Thread* t, int sig, i32 code); // tgkill/tkill: SI_TKILL
void wake_all_threads(proc::Process* p);     // make every sleeping thread re-check (group exit)

// Does `p` have a signal that would actually do something if delivered?
// Discards pending signals that are ignored.
bool pending(proc::Process* p);

// For blocking system calls: should the current wait end with -EINTR?
bool interrupted();

// signalfd: signals pending for the calling thread (its own + process-wide),
// and taking one of them (lowest number in `mask`; 0 if none).
u64 pending_set();
int dequeue(u64 mask);

// On the way back to user mode: run the default action or set up a
// handler frame on the user stack.
void deliver(interrupts::Frame* frame);

// rt_sigreturn: restore the context saved by deliver().
long sigreturn(interrupts::Frame* frame);

// Alternate signal stacks (sigaltstack, SA_ONSTACK), per thread.
constexpr u32 SS_ONSTACK = 1, SS_DISABLE = 2, SS_AUTODISARM = 1u << 31;
constexpr u64 SA_ONSTACK = 0x08000000;
struct AltStack {
    u64 sp;
    u32 flags;
    u64 size;
};
// sigaltstack(2): install *ns (if given), report the previous one in *os.
// `user_sp` is the stack pointer the thread had in user mode.
long sigaltstack(const AltStack* ns, AltStack* os, u64 user_sp);
// For a handler frame: the stack to build it on (the top of the alternate
// stack when the action has SA_ONSTACK and the thread isn't on it already,
// else `user_sp`; *switched says which), and the uc_stack value to save.
// Disarms an SS_AUTODISARM stack, as Linux does.
u64 handler_stack(sched::Thread* t, u64 act_flags, u64 user_sp, bool* switched, AltStack* uc_stack);
// rt_sigreturn: re-install the uc_stack saved in the frame (errors ignored).
void restore_altstack(const AltStack& uc_stack, u64 user_sp);

} // namespace signal
