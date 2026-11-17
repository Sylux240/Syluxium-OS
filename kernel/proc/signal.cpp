#include <fs/poll.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/fpu.hpp>
#include <arch/x86_64/gdt.hpp>
#include <lib/errno.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <mm/layout.hpp>
#include <proc/compat.hpp>
#include <proc/process.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>
#include <sched/sched.hpp>

namespace signal {

namespace {

constexpr u64 SIG_DFL = 0, SIG_IGN = 1;
constexpr u64 SA_SIGINFO = 0x4, SA_RESTORER = 0x04000000, SA_RESTART = 0x10000000, SA_NODEFER = 0x40000000,
              SA_RESETHAND = 0x80000000ull;

// Kernel structures for the handler frame (Linux x86_64 ABI).
struct SigContext {
    u64 r8, r9, r10, r11, r12, r13, r14, r15;
    u64 rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    u16 cs, gs, fs, ss;
    u64 err, trapno, oldmask, cr2;
    u64 fpstate;
    u64 reserved[8];
};
static_assert(sizeof(SigContext) == 256);

struct StackT {
    u64 ss_sp;
    i32 ss_flags;
    i32 pad;
    u64 ss_size;
};

struct UContext {
    u64 uc_flags;
    u64 uc_link;
    StackT uc_stack;
    SigContext mcontext;
    u64 sigmask;
};

struct SigInfo {
    i32 si_signo, si_errno, si_code, pad;
    u8 fields[112];
};
static_assert(sizeof(SigInfo) == 128);

struct RtSigFrame {
    u64 pretcode; // return address: the handler's `ret` lands in the sa_restorer trampoline
    UContext uc;
    SigInfo info;
};

constexpr u64 bit(int sig) { return 1ull << (sig - 1); }
constexpr u64 UNBLOCKABLE = bit(SIGKILL) | bit(SIGSTOP);

bool ignored_by_default(int sig) {
    switch (sig) {
    case SIGCHLD: case SIGCONT: case SIGURG: case SIGWINCH:
    // Stop signals: there's no job-control "stopped" state yet, so they do nothing.
    case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:
        return true;
    default:
        return false;
    }
}

bool is_ignored(proc::Process* p, int sig) {
    if (sig == SIGKILL)
        return false;
    u64 h = p->sigactions[sig].handler;
    return h == SIG_IGN || (h == SIG_DFL && ignored_by_default(sig));
}

// Wake every sleeping thread of `p` so it re-checks for signals.
void wake_process(proc::Process* p) {
    LockGuard guard(p->thread_lock);
    for (sched::Thread* t : p->threads)
        if (t->state == sched::ThreadState::Blocked)
            sched::wake(t);
}

// The whole process is going away (exit_group, fatal signal, execve): this thread must exit.
bool must_exit(proc::Process* p, sched::Thread* t) {
    return p->group_exiting || (p->zap_except && p->zap_except != t);
}

constexpr u64 FIX_EFLAGS = 0x50DD5; // AC OF DF TF SF ZF AF PF CF RF: what sigreturn may change

// While a handler runs: block its sa_mask and (unless SA_NODEFER) the signal itself.
void block_for_handler(proc::Process* p, sched::Thread* t, int sig, const proc::SigAction& act) {
    t->sigmask |= act.mask;
    if (!(act.flags & SA_NODEFER))
        t->sigmask |= bit(sig);
    t->sigmask &= ~UNBLOCKABLE;
    if (act.flags & SA_RESETHAND)
        p->sigactions[sig].handler = SIG_DFL;
}

} // namespace

// Linux keeps a signal whose default action is "ignore" pending while some
// thread blocks it (that's how signalfd receives SIGCHLD). Explicit SIG_IGN
// still discards it.
bool blocked_somewhere(proc::Process* p, int sig) {
    LockGuard guard(p->thread_lock);
    for (sched::Thread* t : p->threads)
        if (t->sigmask & bit(sig))
            return true;
    return false;
}

void send(proc::Process* p, int sig) {
    if (!p || sig < 1 || sig > proc::MAX_SIGNALS || p->state == proc::State::Zombie)
        return;
    if (is_ignored(p, sig) && (p->sigactions[sig].handler == SIG_IGN || !blocked_somewhere(p, sig)))
        return; // would be discarded on delivery anyway
    __atomic_or_fetch(&p->sig_pending, bit(sig), __ATOMIC_RELEASE);
    wake_process(p);
    poll::notify(); // signalfd readers
}

void send_thread(sched::Thread* t, int sig) {
    proc::Process* p = t->process;
    if (!p || sig < 1 || sig > proc::MAX_SIGNALS)
        return;
    if (is_ignored(p, sig) && (p->sigactions[sig].handler == SIG_IGN || !(t->sigmask & bit(sig))))
        return;
    __atomic_or_fetch(&t->sig_pending, bit(sig), __ATOMIC_RELEASE);
    if (t->state == sched::ThreadState::Blocked)
        sched::wake(t);
    poll::notify();
}

void wake_all_threads(proc::Process* p) { wake_process(p); }

void send_to_group(int pgid, int sig) {
    struct Ctx {
        int pgid, sig;
    } ctx{pgid, sig};
    proc::for_each([](proc::Process* p, void* arg) {
        auto* c = static_cast<Ctx*>(arg);
        if (p->pgid == c->pgid)
            send(p, c->sig);
    }, &ctx);
}

bool pending(proc::Process* p) {
    if (!p)
        return false;
    sched::Thread* t = sched::current();
    if (must_exit(p, t))
        return true;
    u64 pend = __atomic_load_n(&p->sig_pending, __ATOMIC_ACQUIRE) | __atomic_load_n(&t->sig_pending, __ATOMIC_ACQUIRE);
    u64 deliverable = pend & (~t->sigmask | UNBLOCKABLE);
    while (deliverable) {
        int sig = __builtin_ctzll(deliverable) + 1;
        deliverable &= ~bit(sig);
        if (is_ignored(p, sig)) {
            __atomic_and_fetch(&p->sig_pending, ~bit(sig), __ATOMIC_RELEASE);
            __atomic_and_fetch(&t->sig_pending, ~bit(sig), __ATOMIC_RELEASE);
        } else {
            return true;
        }
    }
    return false;
}

bool interrupted() { return pending(proc::current()); }

u64 pending_set() {
    proc::Process* p = proc::current();
    sched::Thread* t = sched::current();
    if (!p)
        return 0;
    return __atomic_load_n(&p->sig_pending, __ATOMIC_ACQUIRE) | __atomic_load_n(&t->sig_pending, __ATOMIC_ACQUIRE);
}

int dequeue(u64 mask) {
    proc::Process* p = proc::current();
    sched::Thread* t = sched::current();
    if (!p)
        return 0;
    for (;;) {
        u64 mine = __atomic_load_n(&t->sig_pending, __ATOMIC_ACQUIRE) & mask;
        u64 shared = __atomic_load_n(&p->sig_pending, __ATOMIC_ACQUIRE) & mask;
        u64* src = mine ? &t->sig_pending : &p->sig_pending;
        u64 set = mine ? mine : shared;
        if (!set)
            return 0;
        int sig = __builtin_ctzll(set) + 1;
        // Only one taker wins when several threads race for the same signal.
        if (__atomic_fetch_and(src, ~bit(sig), __ATOMIC_ACQ_REL) & bit(sig))
            return sig;
    }
}

void deliver(interrupts::Frame* frame) {
    proc::Process* p = proc::current();
    sched::Thread* t = sched::current();
    if (!pending(p)) {
        if (t->restore_sigmask) { // temporary mask of ppoll & co.: no signal came, put it back
            t->sigmask = t->saved_sigmask;
            t->restore_sigmask = false;
        }
        return;
    }
    if (must_exit(p, t)) {
        arch::sti();
        proc::exit_thread(0); // another thread ended the process (or is exec'ing)
    }

    // Thread-directed signals first, then process-wide ones.
    u64 mine = __atomic_load_n(&t->sig_pending, __ATOMIC_ACQUIRE) & (~t->sigmask | UNBLOCKABLE);
    u64 shared = __atomic_load_n(&p->sig_pending, __ATOMIC_ACQUIRE) & (~t->sigmask | UNBLOCKABLE);
    int sig;
    if (mine) {
        sig = __builtin_ctzll(mine) + 1;
        __atomic_and_fetch(&t->sig_pending, ~bit(sig), __ATOMIC_RELEASE);
    } else {
        sig = __builtin_ctzll(shared) + 1;
        __atomic_and_fetch(&p->sig_pending, ~bit(sig), __ATOMIC_RELEASE);
    }
    proc::SigAction act = p->sigactions[sig];

    if (act.handler == SIG_DFL || sig == SIGKILL) {
        arch::sti();
        proc::exit_group(sig); // default action: terminate the whole process ("killed by signal")
    }

    // The mask sigreturn restores: the one from before a temporary
    // ppoll/pselect/epoll_pwait mask, if one is active.
    u64 restore_mask = t->restore_sigmask ? t->saved_sigmask : t->sigmask;
    t->restore_sigmask = false;

    // A syscall interrupted by this signal: restart it transparently if the
    // handler asked for SA_RESTART (orig syscall number is kept in error_code).
    if (frame->vector == 0x80 && static_cast<long>(frame->rax) == -err::INTR && (act.flags & SA_RESTART)) {
        frame->rax = frame->error_code;
        frame->rip -= 2; // back over the 2-byte `syscall` instruction
    }

    if (p->compat) {
        // 32-bit program: Linux's ia32 sigframe / rt_sigframe layout.
        u64 blocked = restore_mask;
        arch::sti(); // writing user memory may fault pages in (and sleep)
        if (!compat::setup_signal_frame(frame, sig, act, blocked))
            proc::exit_current(SIGSEGV);
        block_for_handler(p, t, sig, act);
        return;
    }

    usize fp_size = fpu::state_size();
    uptr sp = frame->rsp - 128;                      // skip the red zone
    sp = align_down<uptr>(sp - fp_size, 64);         // FPU/AVX state
    uptr fp_addr = sp;
    sp = align_down<uptr>(sp - sizeof(RtSigFrame), 16) - 8; // handler sees an ABI-aligned stack

    RtSigFrame f{};
    f.pretcode = act.restorer;
    SigContext& c = f.uc.mcontext;
    c.r8 = frame->r8; c.r9 = frame->r9; c.r10 = frame->r10; c.r11 = frame->r11;
    c.r12 = frame->r12; c.r13 = frame->r13; c.r14 = frame->r14; c.r15 = frame->r15;
    c.rdi = frame->rdi; c.rsi = frame->rsi; c.rbp = frame->rbp; c.rbx = frame->rbx;
    c.rdx = frame->rdx; c.rax = frame->rax; c.rcx = frame->rcx; c.rsp = frame->rsp;
    c.rip = frame->rip; c.eflags = frame->rflags;
    c.cs = gdt::USER_CODE; c.ss = gdt::USER_DATA;
    c.oldmask = restore_mask;
    c.fpstate = fp_addr;
    f.uc.sigmask = restore_mask;
    f.info.si_signo = sig;

    fpu::save(t->fpu_state);
    arch::sti(); // writing user memory may fault pages in (and sleep)
    if (!(act.flags & SA_RESTORER) || uaccess::copy_to(reinterpret_cast<void*>(fp_addr), t->fpu_state, fp_size) < 0 ||
        uaccess::copy_to(reinterpret_cast<void*>(sp), &f, sizeof(f)) < 0) {
        proc::exit_current(SIGSEGV); // can't build the frame: like Linux, the process dies
    }

    block_for_handler(p, t, sig, act);

    frame->rip = act.handler;
    frame->rsp = sp;
    frame->rdi = static_cast<u64>(sig);
    frame->rsi = sp + __builtin_offsetof(RtSigFrame, info);
    frame->rdx = sp + __builtin_offsetof(RtSigFrame, uc);
    frame->rax = 0;
    frame->rflags &= ~(0x400ull | 0x100ull); // clear DF and TF for the handler
    (void)SA_SIGINFO;
}

long sigreturn(interrupts::Frame* frame) {
    sched::Thread* t = sched::current();
    uptr base = frame->rsp - 8; // the handler's `ret` popped pretcode

    RtSigFrame f;
    if (uaccess::copy_from(&f, reinterpret_cast<void*>(base), sizeof(f)) < 0)
        proc::exit_current(SIGSEGV);
    const SigContext& c = f.uc.mcontext;
    if (c.rip >= mm::USER_END)
        proc::exit_current(SIGSEGV);

    frame->r8 = c.r8; frame->r9 = c.r9; frame->r10 = c.r10; frame->r11 = c.r11;
    frame->r12 = c.r12; frame->r13 = c.r13; frame->r14 = c.r14; frame->r15 = c.r15;
    frame->rdi = c.rdi; frame->rsi = c.rsi; frame->rbp = c.rbp; frame->rbx = c.rbx;
    frame->rdx = c.rdx; frame->rax = c.rax; frame->rcx = c.rcx; frame->rsp = c.rsp;
    frame->rip = c.rip;
    frame->rflags = (frame->rflags & ~FIX_EFLAGS) | (c.eflags & FIX_EFLAGS) | arch::RFLAGS_IF;
    frame->cs = gdt::USER_CODE;
    frame->ss = gdt::USER_DATA;
    frame->vector = 0; // not a restartable syscall frame any more
    t->sigmask = f.uc.sigmask & ~UNBLOCKABLE;

    if (c.fpstate) {
        usize size = fpu::state_size();
        auto* tmp = static_cast<u8*>(kmalloc(size));
        if (tmp && uaccess::copy_from(tmp, reinterpret_cast<void*>(c.fpstate), size) == 0) {
            fpu::sanitize(tmp);
            memcpy(t->fpu_state, tmp, size);
            fpu::restore(t->fpu_state);
        }
        kfree(tmp);
    }
    return static_cast<long>(frame->rax); // syscall_dispatch stores this back into rax
}

} // namespace signal
