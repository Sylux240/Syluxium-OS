#include <video/splash.hpp>
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
    splash::panic_off(); // the panic must be visible
    kprintf("\n\x1b[97;41m KERNEL PANIC \x1b[0m (CPU %u) \x1b[91m", this_cpu()->index);
    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
    kprintf("\x1b[0m\n");
    // Call chain (the kernel keeps frame pointers): addr2line -e build/syluxium.elf <addresses>
    kprintf("Backtrace:");
    const u64* fp = static_cast<const u64*>(__builtin_frame_address(0));
    for (int i = 0; i < 16 && reinterpret_cast<u64>(fp) >= 0xFFFF800000000000ull &&
                    (reinterpret_cast<u64>(fp) & 7) == 0; i++) {
        u64 ret = fp[1];
        if (ret < 0xFFFF800000000000ull)
            break;
        kprintf(" %016lx", ret);
        const u64* next = reinterpret_cast<const u64*>(fp[0]);
        if (next <= fp)
            break;
        fp = next;
    }
    kprintf("\n\nSystem halted.\n");

    arch::halt_forever();
}
