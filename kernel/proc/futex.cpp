#include <lib/errno.hpp>
#include <lib/spinlock.hpp>
#include <proc/futex.hpp>
#include <proc/process.hpp>
#include <proc/signal.hpp>
#include <proc/uaccess.hpp>
#include <sched/sched.hpp>
#include <time/clock.hpp>

namespace futex {

namespace {

// Waiters hash into buckets by key. Private futexes are keyed by
// (address space, virtual address); shared ones by physical address, so
// processes mapping the same memory meet at the same futex.
struct Bucket {
    IrqSpinlock lock;
    sched::WaitList waiters;
};

constexpr usize BUCKETS = 256;
Bucket g_buckets[BUCKETS];

struct Key {
    u64 a, b;
    bool operator==(const Key&) const = default;
};

long make_key(u64 uaddr, bool is_private, Key* out) {
    if (uaddr & 3)
        return -err::INVAL;
    if (long r = uaccess::check(reinterpret_cast<void*>(uaddr), 4, false); r < 0)
        return r;
    proc::Process* p = proc::current();
    if (is_private) {
        *out = Key{reinterpret_cast<u64>(p->mem), uaddr};
        return 0;
    }
    u64 phys;
    if (!p->mem->aspace()->translate(uaddr, &phys))
        return -err::FAULT;
    *out = Key{phys, 1};
    return 0;
}

Bucket& bucket_for(const Key& k) {
    u64 h = (k.a * 0x9E3779B97F4A7C15ull) ^ (k.b * 0xC2B2AE3D27D4EB4Full);
    return g_buckets[(h >> 32) % BUCKETS];
}

bool matches(const sched::Thread* t, const Key& k) { return t->futex_key_a == k.a && t->futex_key_b == k.b; }

} // namespace

long wait(u64 uaddr, u32 expected, u64 deadline_ns, u32 bitset, bool is_private) {
    if (bitset == 0)
        return -err::INVAL;
    Key key;
    if (long r = make_key(uaddr, is_private, &key); r < 0)
        return r;
    Bucket& b = bucket_for(key);
    sched::Thread* me = sched::current();

    b.lock.lock();
    // The word is checked under the bucket lock, so a waker that changes it
    // and then calls wake() can't slip in between the check and the sleep.
    u32 current = *reinterpret_cast<volatile u32*>(uaddr);
    if (current != expected) {
        b.lock.unlock();
        return -err::AGAIN;
    }
    if (signal::interrupted()) {
        b.lock.unlock();
        return -err::INTR;
    }
    me->futex_key_a = key.a;
    me->futex_key_b = key.b;
    me->futex_bitset = bitset;
    bool woken = sched::block_on(b.waiters, b.lock, deadline_ns);
    me->futex_key_a = me->futex_key_b = 0;
    b.lock.unlock();

    if (!woken)
        return -err::TIMEDOUT;
    return 0; // includes spurious wakeups (signals); callers always re-check the word
}

long wake(u64 uaddr, int count, u32 bitset, bool is_private) {
    Key key;
    if (long r = make_key(uaddr, is_private, &key); r < 0)
        return r;
    Bucket& b = bucket_for(key);
    long woken = 0;
    LockGuard guard(b.lock);
    for (sched::Thread* t = b.waiters.front(); t && woken < count;) {
        sched::Thread* next = b.waiters.next_of(t);
        if (matches(t, key) && (t->futex_bitset & bitset)) {
            b.waiters.remove(t);
            // Woken while we still hold the bucket lock: the sleeper re-takes
            // it before returning, so it can't exit (and be freed) under us.
            sched::wake(t);
            woken++;
        }
        t = next;
    }
    return woken;
}

long requeue(u64 uaddr, u64 uaddr2, int wake_count, int requeue_count, bool compare, u32 expected,
             bool is_private) {
    // Moving sleepers to the second futex would leave them holding the
    // wrong bucket lock when they wake. Waking them instead is always
    // correct (futex users must tolerate spurious wakeups); modern glibc
    // doesn't requeue for ordinary condition variables anyway.
    (void)uaddr2;
    if (compare) {
        if (long r = uaccess::check(reinterpret_cast<void*>(uaddr), 4, false); r < 0)
            return r;
        if (*reinterpret_cast<volatile u32*>(uaddr) != expected)
            return -err::AGAIN;
    }
    long total = static_cast<long>(wake_count) + static_cast<long>(requeue_count);
    return wake(uaddr, total > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(total), ~0u, is_private);
}

} // namespace futex
