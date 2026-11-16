#pragma once

#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/cpu_local.hpp>

// Bare test-and-set lock. Doesn't touch preemption or interrupts; only for
// code that already runs with interrupts off (scheduler internals).
class RawSpinlock {
public:
    void lock() {
        while (__atomic_test_and_set(&m_locked, __ATOMIC_ACQUIRE)) {
            while (__atomic_load_n(&m_locked, __ATOMIC_RELAXED))
                arch::pause();
        }
    }

    bool try_lock() { return !__atomic_test_and_set(&m_locked, __ATOMIC_ACQUIRE); }
    void unlock() { __atomic_clear(&m_locked, __ATOMIC_RELEASE); }
    bool is_locked() const { return __atomic_load_n(&m_locked, __ATOMIC_RELAXED); }

private:
    bool m_locked = false;
};

// Spinlock for short critical sections. Disables preemption while held,
// so the holder can't be switched out and leave others spinning.
// Not safe against interrupt handlers on the same CPU: use IrqSpinlock for that.
class Spinlock {
public:
    void lock() {
        cpu_local::preempt_disable();
        m_lock.lock();
    }

    bool try_lock() {
        cpu_local::preempt_disable();
        if (m_lock.try_lock())
            return true;
        cpu_local::preempt_dec();
        return false;
    }

    void unlock() {
        m_lock.unlock();
        if (cpu_local::preempt_dec() && cpu_local::need_resched() && arch::interrupts_enabled())
            sched_preempt_point();
    }

    bool is_locked() const { return m_lock.is_locked(); }

private:
    RawSpinlock m_lock;
};

// Spinlock that also disables interrupts while held, so an interrupt
// handler on the same CPU can't deadlock trying to take it.
class IrqSpinlock {
public:
    void lock() {
        bool was_enabled = arch::interrupts_enabled();
        arch::cli();
        cpu_local::preempt_disable();
        m_lock.lock();
        m_restore_interrupts = was_enabled;
    }

    void unlock() {
        bool restore = m_restore_interrupts;
        m_lock.unlock();
        bool zero = cpu_local::preempt_dec();
        if (restore) {
            arch::sti();
            if (zero && cpu_local::need_resched())
                sched_preempt_point();
        }
    }

    // Used by panic(): forget whoever held the lock so we can still print.
    void force_unlock() { m_lock.unlock(); }

    bool is_locked() const { return m_lock.is_locked(); }

private:
    RawSpinlock m_lock;
    bool m_restore_interrupts = false;
};

template <typename Lock>
class LockGuard {
public:
    explicit LockGuard(Lock& lock) : m_lock(lock) { m_lock.lock(); }
    ~LockGuard() { m_lock.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Lock& m_lock;
};

// Save interrupt state and disable; restore later.
class InterruptGuard {
public:
    InterruptGuard() : m_was_enabled(arch::interrupts_enabled()) { arch::cli(); }
    ~InterruptGuard() {
        if (m_was_enabled)
            arch::sti();
    }

private:
    bool m_was_enabled;
};
