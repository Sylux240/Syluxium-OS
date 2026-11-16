#include <arch/x86_64/apic.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/percpu.hpp>
#include <core/log.hpp>
#include <core/panic.hpp>

[[noreturn]] void panic(const char* fmt, ...) {
    arch::cli();

    // Only the first CPU to panic prints; everyone else just stops.
    static bool in_panic = false;
    if (__atomic_exchange_n(&in_panic, true, __ATOMIC_ACQ_REL))
        arch::halt_forever();
    if (percpu::online_count() > 1)
        apic::broadcast_ipi_others(interrupts::VEC_PANIC_HALT);

    klog::break_lock();
    kprintf("\n\x1b[97;41m KERNEL PANIC \x1b[0m (CPU %u) \x1b[91m", this_cpu()->index);
    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
    kprintf("\x1b[0m\n\nSystem halted.\n");

    arch::halt_forever();
}
