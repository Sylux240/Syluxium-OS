#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/gdt.hpp>
#include <core/log.hpp>
#include <proc/process.hpp>
#include <proc/signal.hpp>
#include <syscall/syscall.hpp>

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
    // (user SS = +8, user CS = +16), i.e. 0x10 -> SS 0x1b, CS 0x23.
    arch::wrmsr(MSR_STAR, (static_cast<u64>(gdt::KERNEL_DATA) << 48) | (static_cast<u64>(gdt::KERNEL_CODE) << 32));
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
    register_compat(); // last: the 32-bit table reuses native handlers
}

Handler native(u32 number) { return number < MAX_SYSCALLS ? g_table[number].handler : nullptr; }

} // namespace syscall

extern "C" void syscall_dispatch(interrupts::Frame* frame) {
    using namespace syscall;
    u64 nr = frame->rax;
    long result;
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
    frame->rax = static_cast<u64>(result);

    // Back to user mode: run pending signals' handlers / default actions.
    if (signal::pending(proc::current()))
        signal::deliver(frame);
}
