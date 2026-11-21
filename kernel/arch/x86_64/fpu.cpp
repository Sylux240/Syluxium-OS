#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/cpu_local.hpp>
#include <arch/x86_64/fpu.hpp>
#include <arch/x86_64/percpu.hpp>
#include <core/panic.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>

namespace fpu {

namespace {

constexpr u64 CR0_MP = 1 << 1, CR0_EM = 1 << 2, CR0_TS = 1 << 3, CR0_NE = 1 << 5;
constexpr u64 CR4_OSFXSR = 1 << 9, CR4_OSXMMEXCPT = 1 << 10, CR4_OSXSAVE = 1 << 18;

// XCR0 user-state components we enable when supported:
// x87, SSE, AVX, AVX-512 opmask, AVX-512 upper ZMM0-15, ZMM16-31.
constexpr u64 XCR0_WANTED = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 5) | (1 << 6) | (1 << 7);

bool g_xsave = false;
u32 g_mxcsr_mask = 0xFFBF;
u64 g_xcr0 = 0;
usize g_size = 512;
alignas(64) u8 g_initial_state[16 * KiB]; // template for new threads

void xsetbv(u32 reg, u64 value) {
    asm volatile("xsetbv" : : "c"(reg), "a"(static_cast<u32>(value)), "d"(static_cast<u32>(value >> 32)));
}

void enable_on_this_cpu() {
    u64 cr0 = arch::read_cr0();
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP | CR0_NE;
    arch::write_cr0(cr0);

    u64 cr4 = arch::read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT;
    if (g_xsave)
        cr4 |= CR4_OSXSAVE;
    arch::write_cr4(cr4);
    if (g_xsave)
        xsetbv(0, g_xcr0);
}

} // namespace

void init_bsp() {
    auto leaf1 = arch::cpuid(1);
    if (!(leaf1.edx & (1u << 25)))
        panic("fpu: CPU has no SSE");
    g_xsave = leaf1.ecx & (1u << 26);

    if (g_xsave) {
        auto d0 = arch::cpuid(0xD, 0);
        u64 supported = (static_cast<u64>(d0.edx) << 32) | d0.eax;
        g_xcr0 = supported & XCR0_WANTED;
        // AVX-512 needs all three of its components, or none.
        if ((g_xcr0 & 0xE0) != 0xE0)
            g_xcr0 &= ~0xE0ull;
    }
    enable_on_this_cpu();

    if (g_xsave) {
        g_size = arch::cpuid(0xD, 0).ebx; // size for the components now enabled in XCR0
        if (g_size > sizeof(g_initial_state))
            panic("fpu: XSAVE area of %zu bytes is larger than supported", g_size);
    }

    // Build the clean initial state: default x87 control word, MXCSR with all
    // exceptions masked (what Linux gives a new process).
    asm volatile("fninit");
    u32 mxcsr = 0x1F80;
    asm volatile("ldmxcsr %0" : : "m"(mxcsr));
    memset(g_initial_state, 0, sizeof(g_initial_state));
    save(g_initial_state);
    u32 mask;
    memcpy(&mask, g_initial_state + 28, 4); // MXCSR_MASK field of the legacy area
    if (mask)
        g_mxcsr_mask = mask;
}

void init_ap() { enable_on_this_cpu(); }

usize state_size() { return g_size; }

void* alloc_state() {
    // kmalloc blocks are aligned to their size class, so >= 64 bytes alignment.
    void* p = kmalloc(g_size < 64 ? 64 : g_size);
    if (!p)
        panic("fpu: out of memory for FPU state");
    memcpy(p, g_initial_state, g_size);
    return p;
}

void free_state(void* state) { kfree(state); }

void reset(void* state) { memcpy(state, g_initial_state, g_size); }

void sanitize(void* state) {
    auto* p = static_cast<u8*>(state);
    // MXCSR with reserved bits set makes FXRSTOR/XRSTOR fault (#GP in the kernel).
    u32 mxcsr;
    memcpy(&mxcsr, p + 24, 4);
    mxcsr &= g_mxcsr_mask;
    memcpy(p + 24, &mxcsr, 4);
    if (g_xsave) {
        // XSAVE header: XSTATE_BV may only name enabled components; XCOMP_BV
        // and the reserved bytes must be zero (standard format).
        u64 bv;
        memcpy(&bv, p + 512, 8);
        bv &= g_xcr0;
        memcpy(p + 512, &bv, 8);
        memset(p + 520, 0, 56);
    }
}

void save(void* state) {
    if (g_xsave)
        asm volatile("xsave64 (%0)" : : "r"(state), "a"(0xFFFFFFFF), "d"(0xFFFFFFFF) : "memory");
    else
        asm volatile("fxsave64 (%0)" : : "r"(state) : "memory");
}

void restore(const void* state) {
    if (g_xsave)
        asm volatile("xrstor64 (%0)" : : "r"(state), "a"(0xFFFFFFFF), "d"(0xFFFFFFFF) : "memory");
    else
        asm volatile("fxrstor64 (%0)" : : "r"(state) : "memory");
}

void kernel_begin() {
    cpu_local::preempt_disable();
    Cpu* cpu = this_cpu();
    if (cpu->kernel_fpu_depth++ == 0) {
        if (!cpu->kernel_fpu_save)
            cpu->kernel_fpu_save = alloc_state();
        save(cpu->kernel_fpu_save);   // whoever's registers these are (eager switching: the current thread's)
        restore(g_initial_state);     // clean x87/SSE/AVX state, default MXCSR (exceptions masked)
    }
}

void kernel_end() {
    Cpu* cpu = this_cpu();
    if (cpu->kernel_fpu_depth && --cpu->kernel_fpu_depth == 0)
        restore(cpu->kernel_fpu_save);
    bool zero = cpu_local::preempt_dec();
    if (zero && cpu_local::need_resched() && arch::interrupts_enabled())
        sched_preempt_point();
}

bool kernel_active() { return this_cpu()->kernel_fpu_depth != 0; }

const char* mode_name() {
    if (!g_xsave)
        return "FXSAVE (SSE)";
    if (g_xcr0 & 0xE0)
        return "XSAVE (SSE, AVX, AVX-512)";
    if (g_xcr0 & 4)
        return "XSAVE (SSE, AVX)";
    return "XSAVE (SSE)";
}

u64 xcr0() { return g_xcr0; }

} // namespace fpu
