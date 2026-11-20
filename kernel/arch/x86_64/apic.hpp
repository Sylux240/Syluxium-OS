#pragma once

#include <lib/types.hpp>

// Local APIC: the per-CPU interrupt controller. Provides the per-CPU timer
// that drives preemption and inter-processor interrupts (IPIs).
// Uses x2APIC mode (MSRs) when the CPU supports it, else xAPIC (MMIO).
namespace apic {

void init_bsp();      // detect mode, map registers, enable on the boot CPU
void init_ap();       // enable on an application processor

u32 local_id();
bool x2apic_mode();
void eoi();

// Timer: calibrate once on the BSP, then start periodic ticks on each CPU.
void calibrate_timer();
void start_timer(u32 hz);
u32 timer_ticks_per_ms();
void timer_oneshot_ns(u64 ns); // one interrupt after `ns` (idle CPUs; start_timer resumes ticking)

void send_ipi(u32 lapic_id, u8 vector);
void send_nmi(u32 lapic_id); // non-maskable: reaches a CPU even with interrupts off
void broadcast_ipi_others(u8 vector);

// Start-up IPI sequence is done by Limine, so we don't need INIT/SIPI here.

} // namespace apic

// I/O APIC: routes device interrupt lines (GSIs) to CPUs and vectors.
namespace ioapic {

void init();  // mask every input on every I/O APIC

// Route a legacy ISA IRQ (0-15), honoring ACPI interrupt source overrides.
bool route_isa_irq(u8 irq, u8 vector, u32 dest_lapic_id);
bool route_gsi(u32 gsi, u8 vector, u32 dest_lapic_id, bool level_triggered, bool active_low);
void mask_gsi(u32 gsi);

u32 count();

} // namespace ioapic
