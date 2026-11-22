#include <arch/x86_64/protect.hpp>
#include <boot/limine_requests.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/gdt.hpp>
#include <core/log.hpp>
#include <lib/string.hpp>
#include <proc/process.hpp>
#include <proc/signal.hpp>
#include <syscall/syscall.hpp>
#include <bpf/bpf.hpp>

extern "C" void syscall_entry();
extern "C" void syscall32_entry(); // compat_entry.S

namespace syscall {

namespace {

constexpr u32 MAX_SYSCALLS = 512;
constexpr u32 MSR_EFER = 0xC0000080, MSR_STAR = 0xC0000081, MSR_LSTAR = 0xC0000082, MSR_CSTAR = 0xC0000083,
              MSR_FMASK = 0xC0000084, MSR_SYSENTER_CS = 0x174;
constexpr u64 EFER_SCE = 1;

struct Entry {
    Handler handler;
    const char* name;
};

Entry g_table[MAX_SYSCALLS];
bool g_warned[MAX_SYSCALLS];

} // namespace

void init_cpu() {
    arch::wrmsr(MSR_EFER, arch::rdmsr(MSR_EFER) | EFER_SCE);
    // STAR[47:32]: kernel CS (SS = +8). STAR[63:48]: base for SYSRET
    // (32-bit user CS = +0, user SS = +8, 64-bit user CS = +16), i.e. Linux's
    // 0x20 -> CS32 0x23, SS 0x2b, CS 0x33. (We return with iretq, but keep it right.)
    static_assert(gdt::USER_DATA == gdt::USER32_CODE + 8 && gdt::USER_CODE == gdt::USER32_CODE + 16);
    arch::wrmsr(MSR_STAR, (static_cast<u64>(gdt::USER32_CODE & ~3) << 48) | (static_cast<u64>(gdt::KERNEL_CODE) << 32));
    arch::wrmsr(MSR_LSTAR, reinterpret_cast<u64>(&syscall_entry));
    // 32-bit programs: SYSCALL from compatibility mode (AMD) enters at CSTAR.
    // SYSENTER stays disabled (CS MSR 0 -> #GP -> SIGSEGV); our vsyscall
    // page uses int $0x80, so nothing needs it.
    arch::wrmsr(MSR_CSTAR, reinterpret_cast<u64>(&syscall32_entry));
    arch::wrmsr(MSR_SYSENTER_CS, 0);
    // Cleared on entry: IF (no interrupts until we're on the kernel stack),
    // DF, TF, AC, NT.
    arch::wrmsr(MSR_FMASK, 0x200 | 0x400 | 0x100 | 0x40000 | 0x4000);
}

void set(u32 number, Handler handler, const char* name) {
    if (number < MAX_SYSCALLS)
        g_table[number] = Entry{handler, name};
}

void register_all() {
    register_file();
    register_memory();
    register_process();
    register_time();
    register_net(); // after register_file: replaces its socket stubs
    register_events();
    register_ipc();
    register_inotify();
    bpf::register_syscalls();
    register_compat(); // last: the 32-bit table reuses native handlers
}

Handler native(u32 number) { return number < MAX_SYSCALLS ? g_table[number].handler : nullptr; }

} // namespace syscall

namespace {
// Debugging aid: "strace=<program name>" on the kernel command line logs
// every system call that program makes (name, arguments, result) to the
// serial console, as it enters and when it returns.
char g_trace_name[32];
int g_trace_state = -1; // -1 = command line not read yet, 0 = off, 1 = on

bool tracing(proc::Process* p) {
    if (g_trace_state < 0) {
        g_trace_state = 0;
        const char* c = boot::cmdline();
        for (const char* q = c; *q; q++) {
            if ((q == c || q[-1] == ' ') && memcmp(q, "strace=", 7) == 0) {
                usize n = 0;
                for (q += 7; *q && *q != ' ' && n + 1 < sizeof(g_trace_name); q++)
                    g_trace_name[n++] = *q;
                g_trace_name[n] = 0;
                g_trace_state = n ? 1 : 0;
                break;
            }
        }
    }
    return g_trace_state == 1 && p && strcmp(p->name, g_trace_name) == 0;
}
} // namespace

extern "C" void syscall_dispatch(interrupts::Frame* frame) {
    using namespace syscall;
    u64 nr = frame->rax;
    long result;
    proc::Process* traced = tracing(proc::current()) ? proc::current() : nullptr;
    if (traced && nr < MAX_SYSCALLS)
        kprintf_serial("[strace %d] %s(%#lx, %#lx, %#lx, %#lx) ...\n", traced->pid,
                       g_table[nr].name ? g_table[nr].name : "?", frame->rdi, frame->rsi, frame->rdx, frame->r10);
    sched::Thread* self = sched::current();
    protect::report_pending();
    self->cur_syscall = static_cast<i64>(nr);
    self->syscall_frame = frame;
    self->cur_args[0] = frame->rdi, self->cur_args[1] = frame->rsi, self->cur_args[2] = frame->rdx;
    if (nr < MAX_SYSCALLS && g_table[nr].handler) {
        result = g_table[nr].handler(frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9, frame);
    } else {
        if (nr < MAX_SYSCALLS && !g_warned[nr]) {
            g_warned[nr] = true;
            proc::Process* p = proc::current();
            klog::warn("%s[%d]: unimplemented syscall %lu", p ? p->name : "?", p ? p->pid : 0, nr);
        }
        result = -err::NOSYS;
    }
    if (traced && nr < MAX_SYSCALLS)
        kprintf_serial("[strace %d]   %s = %ld\n", traced->pid, g_table[nr].name ? g_table[nr].name : "?", result);
    self->cur_syscall = -1;
    frame->rax = static_cast<u64>(result);

    // Back to user mode: run pending signals' handlers / default actions.
    if (signal::pending(proc::current()))
        signal::deliver(frame);
}
