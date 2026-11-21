// Kernel FPU sections (fpu::kernel_begin/end): the registers live before a
// section come back bit for bit afterwards, a section starts from the clean
// state, sections nest. With "fputest" on the command line every CPU also
// hammers kernel sections for 60 s (run user-space FP work meanwhile, e.g.
// /opt/syluxium/tests/fputest, to catch any mix-up with user state).
#include <arch/x86_64/fpu.hpp>
#include <arch/x86_64/cpu_local.hpp>
#include <arch/x86_64/percpu.hpp>
#include <boot/limine_requests.hpp>
#include <core/log.hpp>
#include <core/panic.hpp>
#include <lib/string.hpp>
#include <sched/sched.hpp>
#include <tests/selftest.hpp>
#include <time/clock.hpp>

namespace selftest {

namespace {

struct alignas(16) XmmFile {
    u64 q[32]; // xmm0..xmm15
    u32 mxcsr;
};

// Raw register access: the kernel is compiled without SSE, so the compiler
// never keeps anything in these registers itself.
void load_xmm(const XmmFile& f) {
    asm volatile(
        "movdqu 0(%0), %%xmm0\n movdqu 16(%0), %%xmm1\n movdqu 32(%0), %%xmm2\n movdqu 48(%0), %%xmm3\n"
        "movdqu 64(%0), %%xmm4\n movdqu 80(%0), %%xmm5\n movdqu 96(%0), %%xmm6\n movdqu 112(%0), %%xmm7\n"
        "movdqu 128(%0), %%xmm8\n movdqu 144(%0), %%xmm9\n movdqu 160(%0), %%xmm10\n movdqu 176(%0), %%xmm11\n"
        "movdqu 192(%0), %%xmm12\n movdqu 208(%0), %%xmm13\n movdqu 224(%0), %%xmm14\n movdqu 240(%0), %%xmm15\n"
        "ldmxcsr 256(%0)\n"
        : : "r"(&f) : "memory");
}

void store_xmm(XmmFile& f) {
    asm volatile(
        "movdqu %%xmm0, 0(%0)\n movdqu %%xmm1, 16(%0)\n movdqu %%xmm2, 32(%0)\n movdqu %%xmm3, 48(%0)\n"
        "movdqu %%xmm4, 64(%0)\n movdqu %%xmm5, 80(%0)\n movdqu %%xmm6, 96(%0)\n movdqu %%xmm7, 112(%0)\n"
        "movdqu %%xmm8, 128(%0)\n movdqu %%xmm9, 144(%0)\n movdqu %%xmm10, 160(%0)\n movdqu %%xmm11, 176(%0)\n"
        "movdqu %%xmm12, 192(%0)\n movdqu %%xmm13, 208(%0)\n movdqu %%xmm14, 224(%0)\n movdqu %%xmm15, 240(%0)\n"
        "stmxcsr 256(%0)\n"
        : : "r"(&f) : "memory");
}

void fill(XmmFile& f, u64 seed) {
    for (int i = 0; i < 32; i++) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        f.q[i] = seed;
    }
    f.mxcsr = 0x1f80 ^ 0x6000; // round toward zero: not the default
}

bool same(const XmmFile& a, const XmmFile& b) { return memcmp(a.q, b.q, sizeof(a.q)) == 0 && a.mxcsr == b.mxcsr; }

// One round: returns false (and says why) on any mismatch.
bool round_trip(u64 seed, const char** why) {
    XmmFile outer, inner, got;
    fill(outer, seed);
    fill(inner, ~seed);
    cpu_local::preempt_disable(); // "outer" must stay live in the registers
    load_xmm(outer);
    fpu::kernel_begin();
    store_xmm(got);
    bool clean = got.mxcsr == 0x1f80;
    for (int i = 0; i < 32; i++)
        clean = clean && got.q[i] == 0;
    load_xmm(inner);
    fpu::kernel_begin(); // nested: no save/restore, registers stay
    store_xmm(got);
    bool nested = same(got, inner);
    fpu::kernel_end();
    fpu::kernel_end();
    store_xmm(got);
    bool restored = same(got, outer);
    bool zero = cpu_local::preempt_dec();
    (void)zero;
    *why = !clean ? "section didn't start from the clean state" : !nested ? "nested section lost registers"
                                                                         : "registers not restored";
    return clean && nested && restored;
}

volatile u64 g_stress_rounds = 0;
volatile u64 g_stress_errors = 0;

void stress(void* arg) {
    u64 seed = reinterpret_cast<u64>(arg) * 0x9e3779b97f4a7c15ull;
    u64 end = clock::now_ms() + 60'000;
    while (clock::now_ms() < end) {
        const char* why;
        if (!round_trip(seed++, &why)) {
            __atomic_add_fetch(&g_stress_errors, 1, __ATOMIC_RELAXED);
            klog::error("fputest: %s", why);
        }
        __atomic_add_fetch(&g_stress_rounds, 1, __ATOMIC_RELAXED);
        if ((seed & 63) == 0)
            sched::yield(); // let user threads interleave
    }
}

void stress_report(void*) {
    sched::sleep_ms(61'000);
    klog::info("fputest: %lu kernel FPU sections on all CPUs, %lu errors", g_stress_rounds, g_stress_errors);
}

} // namespace

void run_fpu_tests() {
    // A failure is reported, not fatal: only GPU drivers (boot option "gpu")
    // use kernel FPU sections, and a self-test must never stop a boot.
    u32 failures = 0;
    const char* first = nullptr;
    for (u64 i = 0; i < 1000; i++) {
        const char* why;
        if (!round_trip(i + 1, &why)) {
            if (!failures++)
                first = why;
        }
    }
    if (failures) {
        klog::error("Kernel FPU self-test: %u of 1000 rounds failed (first: %s); GPU drivers may misbehave", failures,
                    first);
        return;
    }
    klog::ok("Kernel FPU sections: save/restore, clean state, nesting (%s)", fpu::mode_name());
    if (strstr(boot::cmdline(), "fputest")) {
        u32 cpus = percpu::online_count();
        for (u32 c = 0; c < cpus; c++)
            sched::spawn("fputest", stress, reinterpret_cast<void*>(static_cast<uptr>(c + 1)), static_cast<int>(c), true);
        sched::spawn("fputest-report", stress_report, nullptr, -1, true);
        klog::info("fputest: kernel FPU stress on %u CPUs for 60 s", cpus);
    }
}

} // namespace selftest
