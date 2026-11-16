#pragma once

#include <lib/types.hpp>
#include <sched/thread.hpp>

class IrqSpinlock;

// Preemptive SMP scheduler.
//
// Each CPU has its own run queue (round robin, 5 ms slices driven by a
// 1 kHz local APIC timer). New threads go to the least loaded CPU; a CPU
// that runs out of work steals runnable threads from busier ones.
namespace sched {

constexpr u32 TICK_HZ = 1000;
constexpr u32 TIME_SLICE_TICKS = 5;
constexpr usize KERNEL_STACK_SIZE = 32 * KiB;

// Create a kernel thread. cpu = -1 lets the scheduler choose (and migrate);
// a specific CPU pins the thread there.
Thread* spawn(const char* name, void (*entry)(void*), void* arg, int cpu = -1, bool detached = false);

// Two-step version of spawn(): create() returns a thread that doesn't run
// yet (so the caller can attach a process / address space), start() makes
// it runnable.
Thread* create(const char* name, void (*entry)(void*), void* arg, bool detached = false);
void start(Thread* thread, int cpu = -1);

[[noreturn]] void exit(int code = 0);
int join(Thread* thread);   // wait for exit, free the thread, return its exit code
void yield();
void sleep_ns(u64 ns);
inline void sleep_ms(u64 ms) { sleep_ns(ms * 1'000'000); }

Thread* current();

// Pick the next thread on this CPU and switch to it.
void schedule();

// Make a blocked thread runnable.
void wake(Thread* thread);

// Block the current thread on `list` (protected by the held `lock`).
// Releases the lock while asleep and re-acquires it before returning.
// deadline_ns = 0 waits forever. Returns false on timeout.
bool block_on(WaitList& list, IrqSpinlock& lock, u64 deadline_ns = 0);

// ---- Startup ----
void init();                                 // reaper + idle thread for the boot CPU
void prepare_cpu(Cpu* cpu);                  // create the idle thread for another CPU
[[noreturn]] void enter();                   // leave the boot stack, start scheduling here
void timer_tick();                           // called from the APIC timer interrupt

bool running();
u64 thread_count();

} // namespace sched
