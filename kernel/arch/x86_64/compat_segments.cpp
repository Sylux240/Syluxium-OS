#include <arch/x86_64/compat_segments.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/gdt.hpp>
#include <arch/x86_64/percpu.hpp>
#include <lib/spinlock.hpp>
#include <lib/string.hpp>

namespace compat_segments {

namespace {

constexpr u32 MSR_FS_BASE = 0xC0000100;
constexpr u32 MSR_GS_BASE = 0xC0000101;

static_assert(sizeof(sched::Thread::tls_desc) / sizeof(u64) == gdt::TLS_ENTRIES);

constexpr u64 DESC_PRESENT = 1ull << 47;

// A selector the user left in FS/GS is reloaded by the kernel, so it must be
// one that can't fault: a ring-0 #GP/#NP here would be fatal. Anything that
// isn't a flat user segment or one of this thread's present TLS descriptors
// becomes the null selector (which is what a bad selector gets on Linux too).
u16 sanitize(const sched::Thread* t, u16 sel) {
    if (sel == 0 || (sel & 7) != 3) // must be in the GDT (TI=0) with RPL 3
        return 0;
    if (sel == gdt::USER_DATA || sel == gdt::USER32_CODE || sel == gdt::USER_CODE)
        return sel;
    int idx = (sel >> 3) - gdt::TLS_FIRST_ENTRY;
    if (idx >= 0 && idx < gdt::TLS_ENTRIES && (t->tls_desc[idx] & DESC_PRESENT))
        return sel;
    return 0;
}

// Loading %gs also loads the GS *base* - which in the kernel is this CPU's
// Cpu pointer. Save and restore it around the load. (An NMI landing between
// the two would see a wrong GS base; the window is a few instructions.)
void load_fs_gs(u16 fs, u16 gs, u64* fs_base, u64* gs_base) {
    u64 kernel_gs = arch::rdmsr(MSR_GS_BASE);
    asm volatile("movw %0, %%fs" : : "r"(fs) : "memory");
    asm volatile("movw %0, %%gs" : : "r"(gs) : "memory");
    if (fs_base)
        *fs_base = fs ? arch::rdmsr(MSR_FS_BASE) : 0;
    if (gs_base)
        *gs_base = gs ? arch::rdmsr(MSR_GS_BASE) : 0;
    arch::wrmsr(MSR_GS_BASE, kernel_gs);
}

void load_data_segments() {
    // Compatibility mode checks DS/ES on every access (64-bit mode ignores
    // them), and an iretq to ring 3 nulls them if they still hold the
    // kernel's DPL-0 data selector. The flat user segment works for both.
    u16 ds = gdt::USER_DATA;
    asm volatile("movw %0, %%ds\n\tmovw %0, %%es" : : "r"(ds) : "memory");
}

} // namespace

u16 read_fs() {
    u16 v;
    asm volatile("movw %%fs, %0" : "=r"(v));
    return v;
}

u16 read_gs() {
    u16 v;
    asm volatile("movw %%gs, %0" : "=r"(v));
    return v;
}

void save(sched::Thread* t) {
    t->user_fs_sel = read_fs();
    t->user_gs_sel = read_gs();
}

void load(Cpu* cpu, sched::Thread* t, bool compat) {
    // DS/ES: flat user data for everyone. 64-bit code ignores them, but a
    // 64-bit process may run 32-bit code too (Wine's WoW64 far-jumps to the
    // 32-bit code segment), and there they're checked on every access. Linux
    // also starts 64-bit programs with DS = ES = __USER_DS.
    load_data_segments();
    // Every user thread gets its own TLS descriptors into this CPU's GDT
    // (as Linux does on every switch): a 64-bit thread may load one of them
    // into %fs itself at any time (Wine, entering 32-bit code), and must never
    // find another thread's descriptor there.
    gdt::install_tls(cpu->tables, t->tls_desc);
    if (!compat && !t->user_fs_sel && !t->user_gs_sel) {
        if (read_fs() | read_gs())
            load_fs_gs(0, 0, nullptr, nullptr); // the caller writes the 64-bit bases next
        return;
    }
    // A 32-bit program, or a 64-bit one that loaded a TLS selector itself
    // (Wine: %fs for its 32-bit threads' TEB, via set_thread_area). A loaded
    // selector's base comes from its descriptor; with the null selector a
    // 64-bit thread keeps its own base (arch_prctl), as on Linux.
    u16 fs = sanitize(t, t->user_fs_sel), gs = sanitize(t, t->user_gs_sel);
    u64 fs_base = 0, gs_base = 0;
    load_fs_gs(fs, gs, &fs_base, &gs_base);
    if (compat || fs)
        t->fs_base = fs_base;
    if (compat || gs)
        t->gs_base = gs_base;
}

void clear_fs() {
    InterruptGuard guard;
    if (read_fs()) {
        u16 zero = 0;
        asm volatile("movw %0, %%fs" : : "r"(zero) : "memory");
    }
}

void clear_gs() {
    InterruptGuard guard;
    if (!read_gs())
        return;
    // As in load_fs_gs: the load clobbers the (kernel's) active GS base.
    u64 kernel_gs = arch::rdmsr(MSR_GS_BASE);
    u16 zero = 0;
    asm volatile("movw %0, %%gs" : : "r"(zero) : "memory");
    arch::wrmsr(MSR_GS_BASE, kernel_gs);
}

void reset(sched::Thread* t, bool compat) {
    InterruptGuard guard;
    memset(t->tls_desc, 0, sizeof(t->tls_desc));
    t->user_fs_sel = t->user_gs_sel = 0;
    load(this_cpu(), t, compat);
}

void install_current(sched::Thread* t) {
    InterruptGuard guard; // no migration between picking the CPU and writing its GDT
    gdt::install_tls(this_cpu()->tables, t->tls_desc);
}

} // namespace compat_segments
