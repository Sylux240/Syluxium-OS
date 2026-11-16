#pragma once

#include <lib/list.hpp>
#include <lib/spinlock.hpp>
#include <lib/types.hpp>

struct Cpu;
namespace vmm { class AddressSpace; }
namespace proc { struct Process; }

namespace sched {

enum class ThreadState : u8 {
    Ready,   // runnable, waiting in a run queue
    Running, // executing on a CPU
    Blocked, // waiting for a wakeup (and/or a timeout)
    Dead,    // exited, waiting for the reaper
};

struct Thread {
    // Must stay the first member: switch.S saves/loads it at offset 0.
    u64 saved_rsp = 0;

    u64 tid = 0;
    char name[32] = {};
    ThreadState state = ThreadState::Blocked;
    bool on_cpu = false;   // executing, or still being switched away from
    bool pinned = false;   // never migrated to another CPU
    bool detached = false; // freed automatically when it exits
    bool reaped = false;   // stack freed; join() may delete the struct
    bool timed_out = false;

    Cpu* cpu = nullptr;    // run queue it belongs to / CPU it last ran on
    void* stack = nullptr;
    usize stack_size = 0;

    void (*entry)(void*) = nullptr;
    void* arg = nullptr;
    int exit_code = 0;

    // User-mode state (null / 0 for pure kernel threads).
    proc::Process* process = nullptr;
    vmm::AddressSpace* aspace = nullptr; // page tables to run with; null = kernel's
    void* fpu_state = nullptr;           // FPU/SSE/AVX registers, saved on switch
    u64 fs_base = 0;                     // TLS pointer (arch_prctl ARCH_SET_FS)
    u64 gs_base = 0;                     // user GS base (swapped out while in the kernel)
    u64 clear_child_tid = 0;             // set_tid_address / CLONE_CHILD_CLEARTID
    int user_tid = 0;                    // Linux thread id (shares the pid number space)
    u64 sig_pending = 0;                 // signals sent to this thread only (tgkill)
    u64 sigmask = 0;                     // blocked signals (per thread, as on Linux)
    u64 futex_key_a = 0, futex_key_b = 0;// futex this thread sleeps on
    u32 futex_bitset = 0;
    u64 utime_ticks = 0, stime_ticks = 0;// CPU time (scheduler ticks) in user / kernel mode

    u64 kernel_stack_top() const { return reinterpret_cast<u64>(stack) + stack_size; }

    u64 wake_deadline = 0; // clock::now_ns() deadline while blocked with a timeout
    u32 slice_left = 0;    // timer ticks before preemption
    u64 context_switches = 0;

    ListNode run_node;     // run queue, or the reaper's list once dead
    ListNode wait_node;    // a WaitList while blocked
    ListNode sleep_node;   // the CPU's timeout list
    ListNode all_node;     // global list of all threads
    ListNode proc_node;    // the owning process's thread list

    // Threads blocked in join() on this thread (linked through their wait_node).
    IntrusiveList<Thread, &Thread::wait_node> join_waiters;
};

using WaitList  = IntrusiveList<Thread, &Thread::wait_node>;
using RunList   = IntrusiveList<Thread, &Thread::run_node>;
using SleepList = IntrusiveList<Thread, &Thread::sleep_node>;
using AllList   = IntrusiveList<Thread, &Thread::all_node>;

} // namespace sched
