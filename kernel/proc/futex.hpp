#pragma once

#include <lib/types.hpp>

// Fast user-space mutexes: the kernel half of every glibc lock, condition
// variable, semaphore and pthread_join. User space spins/atomically updates
// a 32-bit word and only calls the kernel to sleep or to wake sleepers.
namespace interrupts { struct Frame; }

namespace futex {

// Page-fault handler: a fault in the futex code's own user accesses (made
// under spinlocks) is reported to it instead of resolved. True = handled.
bool fault_fixup(interrupts::Frame* frame);

// FUTEX_WAIT: sleep if *uaddr == expected. deadline_ns: clock::now_ns()
// deadline, 0 = forever. Returns 0, -EAGAIN, -ETIMEDOUT or -EINTR.
long wait(u64 uaddr, u32 expected, u64 deadline_ns, u32 bitset, bool is_private);

// Waits that ended because the word changed with nobody waking the sleeper
// (each wait re-checks its word every second).
u64 lost_wakeups();

// FUTEX_WAKE: wake up to `count` waiters whose bitset intersects. Returns how many.
long wake(u64 uaddr, int count, u32 bitset, bool is_private);

// futex_waitv: sleep until one of `n` futexes is woken. Returns its index,
// -EAGAIN (a word didn't hold its expected value), -ETIMEDOUT or -EINTR.
struct WaitvRequest {
    u64 uaddr;
    u32 expected;
    bool is_private;
};
long waitv(const WaitvRequest* req, u32 n, u64 deadline_ns);

// FUTEX_WAKE_OP: atomically change the word at uaddr2 (op/cmp encoded in
// val3), wake up to nr_wake waiters of uaddr and, if the old value of
// uaddr2 passes the comparison, up to nr_wake2 of uaddr2.
long wake_op(u64 uaddr, int nr_wake, u64 uaddr2, int nr_wake2, u32 val3, bool is_private);

// Priority-inheritance futexes (PTHREAD_PRIO_INHERIT mutexes: PulseAudio,
// PipeWire, JACK): the word holds the owner's thread id. FUTEX_LOCK_PI /
// TRYLOCK_PI take it (sleeping while it's owned; deadline as for wait()),
// FUTEX_UNLOCK_PI hands it to the first waiter. No priority boosting: the
// scheduler has no real-time priorities to lend.
long lock_pi(u64 uaddr, u64 deadline_ns, bool try_only, bool is_private);
long unlock_pi(u64 uaddr, bool is_private);

// Thread exit: every robust mutex (set_robust_list) the thread still holds
// gets FUTEX_OWNER_DIED and one waiter is woken, so it can recover the lock.
void exit_robust_list(u64 head, int tid);

// FUTEX_(CMP_)REQUEUE: wake `wake_count`, move up to `requeue_count` more to uaddr2.
long requeue(u64 uaddr, u64 uaddr2, int wake_count, int requeue_count, bool compare, u32 expected,
             bool is_private);

} // namespace futex
