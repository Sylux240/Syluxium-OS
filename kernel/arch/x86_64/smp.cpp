#include <arch/x86_64/protect.hpp>
#include <power/cpupower.hpp>
#include <arch/x86_64/apic.hpp>
#include <arch/x86_64/fpu.hpp>
#include <arch/x86_64/gdt.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/percpu.hpp>
#include <arch/x86_64/smp.hpp>
#include <boot/limine_requests.hpp>
#include <core/log.hpp>
#include <core/panic.hpp>
#include <mm/vmalloc.hpp>
#include <mm/vmm.hpp>
#include <sched/sched.hpp>
#include <syscall/syscall.hpp>
#include <time/clock.hpp>

namespace {

// Each AP starts here, on a small Limine-provided stack, with Limine's page
// tables, interrupts off and GS base 0.
[[noreturn]] void ap_entry(limine_mp_info* info) {
    auto* cpu = reinterpret_cast<Cpu*>(info->extra_argument);

    vmm::init_ap();          // our page tables, PAT, NX, global pages
    percpu::install(cpu);    // this_cpu() works from here on
    gdt::load(cpu->tables, cpu->ist_tops);
    interrupts::load_idt();
    fpu::init_ap();
    protect::init_this_cpu();
    syscall::init_cpu();
    apic::init_ap();
    cpupower::init_cpu();
    apic::start_timer(sched::TICK_HZ);

    sched::enter();          // switch to this CPU's idle thread; marks us online
}

} // namespace

namespace smp {

u32 start_aps() {
    auto* mp = boot::mp();
    if (!mp || mp->cpu_count <= 1)
        return 1;

    u32 next_index = 1;
    for (u64 i = 0; i < mp->cpu_count; i++) {
        limine_mp_info* info = mp->cpus[i];
        if (info->lapic_id == mp->bsp_lapic_id)
            continue;

        Cpu* cpu = percpu::create(next_index++, info->lapic_id);
        for (int k = 0; k < gdt::IST_COUNT; k++) {
            void* stack = vmalloc::alloc(gdt::IST_STACK_SIZE);
            if (!stack)
                panic("smp: out of memory for IST stacks");
            cpu->ist_tops[k] = reinterpret_cast<u64>(stack) + gdt::IST_STACK_SIZE;
        }
        sched::prepare_cpu(cpu);

        info->extra_argument = reinterpret_cast<u64>(cpu);
        // Writing goto_address is what releases the AP.
        __atomic_store_n(&info->goto_address, &ap_entry, __ATOMIC_RELEASE);
    }

    u64 deadline = clock::now_ns() + 2'000'000'000;
    while (percpu::online_count() < next_index && clock::now_ns() < deadline)
        sched::sleep_ms(1);

    u32 online = percpu::online_count();
    if (online < next_index)
        klog::warn("smp: only %u of %u CPUs came online", online, next_index);
    return online;
}

} // namespace smp
