#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/percpu.hpp>
#include <core/panic.hpp>
#include <lib/string.hpp>

namespace percpu {

namespace {

constexpr u32 MSR_GS_BASE = 0xC0000101;
constexpr u32 MSR_KERNEL_GS_BASE = 0xC0000102;
constexpr u32 MAX_CPUS = 256;

constinit Cpu g_bsp; // constinit: must not be re-initialized by global constructors later
Cpu* g_cpus[MAX_CPUS];
u32 g_count = 0;

void init_cpu(Cpu* cpu, u32 index, u32 lapic_id) {
    cpu->self = cpu;
    cpu->index = index;
    cpu->lapic_id = lapic_id;
    cpu->current = &cpu->boot_thread;
    cpu->boot_thread.cpu = cpu;
    cpu->boot_thread.on_cpu = true;
    cpu->boot_thread.state = sched::ThreadState::Running;
    memcpy(cpu->boot_thread.name, "boot", 5);
}

} // namespace

void install(Cpu* cpu) {
    arch::wrmsr(MSR_GS_BASE, reinterpret_cast<u64>(cpu));
    arch::wrmsr(MSR_KERNEL_GS_BASE, 0); // user GS base, swapped in on syscalls later
}

void init_bsp() {
    init_cpu(&g_bsp, 0, 0); // LAPIC id filled in by apic::init_bsp()
    g_cpus[0] = &g_bsp;
    g_count = 1;
    install(&g_bsp);
}

Cpu* create(u32 index, u32 lapic_id) {
    if (index >= MAX_CPUS)
        panic("percpu: more than %u CPUs are not supported", MAX_CPUS);
    Cpu* cpu = new Cpu;
    init_cpu(cpu, index, lapic_id);
    g_cpus[index] = cpu;
    __atomic_store_n(&g_count, max(g_count, index + 1), __ATOMIC_RELEASE);
    return cpu;
}

Cpu* get(u32 index) { return index < g_count ? g_cpus[index] : nullptr; }

u32 count() { return __atomic_load_n(&g_count, __ATOMIC_ACQUIRE); }

u32 online_count() {
    u32 n = 0;
    for (u32 i = 0; i < count(); i++)
        if (g_cpus[i] && __atomic_load_n(&g_cpus[i]->online, __ATOMIC_ACQUIRE))
            n++;
    return n;
}

} // namespace percpu
