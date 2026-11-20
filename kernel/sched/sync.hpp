#pragma once

#include <lib/spinlock.hpp>
#include <sched/sched.hpp>
#include <time/clock.hpp>

// Sleeping synchronization primitives. Semantics follow Linux's
// (wait_event, mutex, semaphore, completion) so Linux driver code can be
// mapped onto them later. All of these may block: never use them while
// holding a spinlock or from an interrupt handler (except the wake/signal/
// complete side, which is interrupt-safe).
namespace sched {

class WaitQueue {
public:
    // Sleep until cond() is true. cond() runs with the queue's lock held.
    template <typename Cond>
    void wait_until(Cond cond) {
        m_lock.lock();
        while (!cond())
            block_on(m_waiters, m_lock);
        m_lock.unlock();
    }

    // Like wait_until, with a timeout. Returns cond()'s final value.
    template <typename Cond>
    bool wait_until_timeout(Cond cond, u64 timeout_ns) {
        u64 deadline = clock::now_ns() + timeout_ns;
        m_lock.lock();
        bool ok;
        while (!(ok = cond()) && clock::now_ns() < deadline)
            block_on(m_waiters, m_lock, deadline);
        m_lock.unlock();
        return ok;
    }

    void wake_one();
    void wake_all();

    // For callers that change the condition under the queue's own lock.
    IrqSpinlock& lock() { return m_lock; }

private:
    IrqSpinlock m_lock;
    WaitList m_waiters;
};

class Mutex {
public:
    void lock();
    bool try_lock();
    void unlock();
    bool held_by_current() const { return m_owner == current(); }
    Thread* owner() const { return m_owner; } // diagnostics

private:
    IrqSpinlock m_lock;
    Thread* m_owner = nullptr;
    WaitList m_waiters;
};

class Semaphore {
public:
    explicit Semaphore(i64 initial = 0) : m_count(initial) {}
    void wait();
    bool try_wait();
    bool wait_timeout(u64 timeout_ns);
    void signal();

private:
    IrqSpinlock m_lock;
    i64 m_count;
    WaitList m_waiters;
};

// One-shot (or reusable via reinit) "this happened" signal.
class Completion {
public:
    void wait();
    bool wait_timeout(u64 timeout_ns);
    void complete();      // release one waiter (or the next wait() call)
    void complete_all();  // release everyone, now and in the future
    void reinit();

private:
    WaitQueue m_queue;
    u64 m_done = 0;
};

} // namespace sched
