#include <core/panic.hpp>
#include <sched/sync.hpp>

namespace sched {

// ---- WaitQueue ----

void WaitQueue::wake_one() {
    m_lock.lock();
    Thread* t = m_waiters.pop_front();
    m_lock.unlock();
    if (t)
        wake(t);
}

void WaitQueue::wake_all() {
    m_lock.lock();
    while (Thread* t = m_waiters.pop_front())
        wake(t);
    m_lock.unlock();
}

// ---- Mutex ----

void Mutex::lock() {
    m_lock.lock();
    if (m_owner == current())
        panic("mutex: recursive lock by thread %s", current()->name);
    while (m_owner)
        block_on(m_waiters, m_lock);
    m_owner = current();
    m_lock.unlock();
}

bool Mutex::try_lock() {
    LockGuard guard(m_lock);
    if (m_owner)
        return false;
    m_owner = current();
    return true;
}

void Mutex::unlock() {
    m_lock.lock();
    if (m_owner != current())
        panic("mutex: unlocked by %s, which doesn't own it", current()->name);
    m_owner = nullptr;
    Thread* next = m_waiters.pop_front();
    m_lock.unlock();
    if (next)
        wake(next);
}

// ---- Semaphore ----

void Semaphore::wait() {
    m_lock.lock();
    while (m_count <= 0)
        block_on(m_waiters, m_lock);
    m_count--;
    m_lock.unlock();
}

bool Semaphore::try_wait() {
    LockGuard guard(m_lock);
    if (m_count <= 0)
        return false;
    m_count--;
    return true;
}

bool Semaphore::wait_timeout(u64 timeout_ns) {
    u64 deadline = clock::now_ns() + timeout_ns;
    m_lock.lock();
    while (m_count <= 0 && clock::now_ns() < deadline)
        block_on(m_waiters, m_lock, deadline);
    bool ok = m_count > 0;
    if (ok)
        m_count--;
    m_lock.unlock();
    return ok;
}

void Semaphore::signal() {
    m_lock.lock();
    m_count++;
    Thread* t = m_waiters.pop_front();
    m_lock.unlock();
    if (t)
        wake(t);
}

// ---- Completion ----

constexpr u64 DONE_ALL = ~u64(0) / 2;

void Completion::wait() {
    m_queue.wait_until([this] {
        if (m_done == 0)
            return false;
        if (m_done != DONE_ALL)
            m_done--;
        return true;
    });
}

bool Completion::wait_timeout(u64 timeout_ns) {
    return m_queue.wait_until_timeout([this] {
        if (m_done == 0)
            return false;
        if (m_done != DONE_ALL)
            m_done--;
        return true;
    }, timeout_ns);
}

void Completion::complete() {
    m_queue.lock().lock();
    if (m_done != DONE_ALL)
        m_done++;
    m_queue.lock().unlock();
    m_queue.wake_one();
}

void Completion::complete_all() {
    m_queue.lock().lock();
    m_done = DONE_ALL;
    m_queue.lock().unlock();
    m_queue.wake_all();
}

void Completion::reinit() {
    LockGuard guard(m_queue.lock());
    m_done = 0;
}

} // namespace sched
