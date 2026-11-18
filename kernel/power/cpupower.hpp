#pragma once

#include <lib/types.hpp>

struct Cpu;

// CPU power management: idle states (C-states) and frequency (P-states).
//
// - Idle: hlt (C1), or MWAIT into the deepest C-state the CPU lists when
//   the APIC timer and TSC keep running there (ARAT + invariant TSC).
//   Idle CPUs stop their periodic tick (see sched's idle loop).
// - Frequency: Intel HWP or AMD CPPC (the CPU picks its own speed from an
//   energy/performance preference), else a load-based governor writing
//   Intel's PERF_CTL ratio (pre-Skylake Intel).
// - /sys/devices/system/cpu/cpuN/cpufreq like Linux.
namespace cpupower {

void init_cpu();      // on every CPU, before it starts scheduling (BSP first)
void publish();       // sysfs files; enables tickless idle (all CPUs online)

bool tickless_enabled();
void idle(u64 expected_ns); // interrupts off on entry; returns after an interrupt, interrupts on
void tick(Cpu* cpu);        // from the scheduler tick (not while tickless)

} // namespace cpupower
