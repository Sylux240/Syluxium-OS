#pragma once

#include <arch/x86_64/cpu_local.hpp>
#include <arch/x86_64/gdt.hpp>
#include <lib/spinlock.hpp>
#include <sched/thread.hpp>

// Per-processor state. Each CPU's GS base points at its own Cpu, so
// this_cpu() is a single instruction and needs no locking.
struct alignas(64) Cpu {
    // ---- Fixed offsets, used from assembly (see cpu_local.hpp) ----
    Cpu* self = this;                  // 0
    sched::Thread* current = nullptr;  // 8
    i32 preempt_count = 0;             // 16
    bool need_resched = false;         // 20
    u64 kernel_stack_top = 0;          // 24
    u64 user_rsp_scratch = 0;          // 32
    // --------------------------------------------------------------

    u32 index = 0;        // 0 = boot CPU, then 1, 2, ...
    u32 lapic_id = 0;
    bool online = false;  // running the scheduler, off the bootloader stack

    gdt::CpuTables tables{};
    u64 ist_tops[gdt::IST_COUNT] = {};

    // Scheduler state for this CPU.
    struct RunQueue {
        RawSpinlock lock;
        sched::RunList ready;      // runnable threads, round robin
        sched::SleepList sleepers; // blocked with a deadline, sorted by deadline
        sched::Thread* idle = nullptr;
    } rq;
    sched::Thread* switch_prev = nullptr; // thread we just switched away from
    sched::Thread boot_thread;            // stands in for the boot context until the scheduler starts

    // Statistics.
    u64 ticks = 0;
    u64 context_switches = 0;
};

static_assert(__builtin_offsetof(Cpu, self) == cpu_local::OFFSET_SELF);
static_assert(__builtin_offsetof(Cpu, current) == cpu_local::OFFSET_CURRENT);
static_assert(__builtin_offsetof(Cpu, preempt_count) == cpu_local::OFFSET_PREEMPT_COUNT);
static_assert(__builtin_offsetof(Cpu, need_resched) == cpu_local::OFFSET_NEED_RESCHED);
static_assert(__builtin_offsetof(Cpu, kernel_stack_top) == cpu_local::OFFSET_KERNEL_STACK);
static_assert(__builtin_offsetof(Cpu, user_rsp_scratch) == cpu_local::OFFSET_USER_RSP);

inline Cpu* this_cpu() {
    Cpu* cpu;
    asm volatile("movq %%gs:0, %0" : "=r"(cpu));
    return cpu;
}

namespace percpu {

// Set up the boot CPU's Cpu and point GS at it. Must run before any lock is used.
void init_bsp();

// Create the Cpu structure for an application processor.
Cpu* create(u32 index, u32 lapic_id);

// Point this CPU's GS base at `cpu`.
void install(Cpu* cpu);

Cpu* get(u32 index);
u32 count();         // CPUs known (created)
u32 online_count();  // CPUs running the scheduler

} // namespace percpu
