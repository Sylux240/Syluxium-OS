#pragma once

#include <arch/x86_64/interrupts.hpp>
#include <lib/types.hpp>

namespace proc {
struct Process;
}

// POSIX signals with Linux numbering and Linux's x86_64 signal-frame layout,
// so glibc's handlers and sigreturn trampolines work unmodified.
namespace signal {

constexpr int SIGHUP = 1, SIGINT = 2, SIGQUIT = 3, SIGILL = 4, SIGTRAP = 5, SIGABRT = 6, SIGBUS = 7,
              SIGFPE = 8, SIGKILL = 9, SIGUSR1 = 10, SIGSEGV = 11, SIGUSR2 = 12, SIGPIPE = 13,
              SIGALRM = 14, SIGTERM = 15, SIGCHLD = 17, SIGCONT = 18, SIGSTOP = 19, SIGTSTP = 20,
              SIGTTIN = 21, SIGTTOU = 22, SIGURG = 23, SIGWINCH = 28;

// Queue a signal for a process and wake it if it's sleeping. Interrupt-safe.
void send(proc::Process* p, int sig);
void send_to_group(int pgid, int sig);

// Does `p` have a signal that would actually do something if delivered?
// Discards pending signals that are ignored.
bool pending(proc::Process* p);

// For blocking system calls: should the current wait end with -EINTR?
bool interrupted();

// On the way back to user mode: run the default action or set up a
// handler frame on the user stack.
void deliver(interrupts::Frame* frame);

// rt_sigreturn: restore the context saved by deliver().
long sigreturn(interrupts::Frame* frame);

} // namespace signal
