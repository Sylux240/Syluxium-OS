#pragma once

#include <lib/types.hpp>

// Fast user-space mutexes: the kernel half of every glibc lock, condition
// variable, semaphore and pthread_join. User space spins/atomically updates
// a 32-bit word and only calls the kernel to sleep or to wake sleepers.
namespace futex {

// FUTEX_WAIT: sleep if *uaddr == expected. deadline_ns: clock::now_ns()
// deadline, 0 = forever. Returns 0, -EAGAIN, -ETIMEDOUT or -EINTR.
long wait(u64 uaddr, u32 expected, u64 deadline_ns, u32 bitset, bool is_private);

// FUTEX_WAKE: wake up to `count` waiters whose bitset intersects. Returns how many.
long wake(u64 uaddr, int count, u32 bitset, bool is_private);

// FUTEX_(CMP_)REQUEUE: wake `wake_count`, move up to `requeue_count` more to uaddr2.
long requeue(u64 uaddr, u64 uaddr2, int wake_count, int requeue_count, bool compare, u32 expected,
             bool is_private);

} // namespace futex
