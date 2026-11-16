#pragma once

#include <lib/types.hpp>

// Fast access to fields of the current CPU's `Cpu` structure through the GS
// segment base (set up by percpu::install). The offsets are checked against
// the real struct layout with static_asserts in percpu.hpp.
namespace cpu_local {

constexpr int OFFSET_SELF          = 0;
constexpr int OFFSET_CURRENT       = 8;
constexpr int OFFSET_PREEMPT_COUNT = 16;
constexpr int OFFSET_NEED_RESCHED  = 20;
constexpr int OFFSET_KERNEL_STACK  = 24; // top of the current thread's kernel stack (syscall entry)
constexpr int OFFSET_USER_RSP      = 32; // scratch: user RSP during syscall entry

inline void preempt_disable() { asm volatile("incl %%gs:16" ::: "memory"); }

// Returns true when the count dropped to zero.
inline bool preempt_dec() {
    bool zero;
    asm volatile("decl %%gs:16" : "=@ccz"(zero) : : "memory");
    return zero;
}

inline i32 preempt_count() {
    i32 v;
    asm volatile("movl %%gs:16, %0" : "=r"(v));
    return v;
}

inline bool need_resched() {
    u8 v;
    asm volatile("movb %%gs:20, %0" : "=r"(v) : : "memory");
    return v;
}

} // namespace cpu_local

// Implemented by the scheduler: reschedule if something asked for it.
extern "C" void sched_preempt_point();
