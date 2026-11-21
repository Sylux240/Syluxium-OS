#include <arch/x86_64/protect.hpp>
#include <core/log.hpp>
#include <lib/errno.hpp>
#include <lib/spinlock.hpp>
#include <mm/heap.hpp>
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

constexpr u64 RECHECK_NS = 1'000'000'000;
u64 g_lost_wakeups = 0;

// Read the futex word (already checked and faulted in by make_key).
u32 user_word(u64 uaddr) {
    protect::UserAccess window;
    return *reinterpret_cast<volatile u32*>(uaddr);
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
    u32 current = user_word(uaddr);
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
    // Sleep in slices of at most a second and look at the word again each
    // time: if it changed while nobody woke us, a wakeup got lost somewhere.
    // Futex waits may return spuriously (every caller re-checks the word),
    // so returning then is always correct -- and it keeps a lost wakeup from
    // freezing the program for good.
    bool woken;
    for (;;) {
        u64 slice = clock::now_ns() + RECHECK_NS;
        bool final = deadline_ns && deadline_ns <= slice;
        woken = sched::block_on(b.waiters, b.lock, final ? deadline_ns : slice,
                                [](void*) { return signal::interrupted(); }, nullptr);
        if (woken || final)
            break;
        if (signal::interrupted()) {
            woken = true; // the caller sees EINTR-like spurious return 0 and re-checks
            break;
        }
        u32 now_val = user_word(uaddr);
        if (now_val != expected) {
            g_lost_wakeups++;
            if (g_lost_wakeups <= 20)
                klog::warn("futex: lost wakeup caught (tid %d, word %#lx: %u -> %u)", me->user_tid, uaddr, expected,
                           now_val);
            woken = true;
            break;
        }
    }
    me->futex_key_a = me->futex_key_b = 0;
    b.lock.unlock();

    if (!woken)
        return -err::TIMEDOUT;
    return 0; // includes spurious wakeups (signals); callers always re-check the word
}

u64 lost_wakeups() { return g_lost_wakeups; }

// ---- futex_waitv: sleep on several futexes at once (Proton's fsync) ----
// Waiters sit on one global list (they're rare); wake() looks through it
// for waiters of the woken futex.
namespace {

struct WaitvEntry {
    Key key;
    u64 uaddr;
    u32 expected;
};

struct Waitv {
    sched::Thread* thread;
    WaitvEntry* entries;
    u32 count;
    int woken = -1; // index of the futex that woke us
    sched::WaitList list;
    Waitv* next = nullptr;
};

IrqSpinlock g_waitv_lock;
Waitv* g_waitv = nullptr;
u32 g_waitv_count = 0;

// Under g_waitv_lock: wake up to `count` waitv sleepers of `key`.
long wake_waitv(const Key& key, long count) {
    long woken = 0;
    for (Waitv* w = g_waitv; w && woken < count; w = w->next) {
        if (w->woken >= 0)
            continue;
        for (u32 i = 0; i < w->count; i++) {
            if (w->entries[i].key == key) {
                w->woken = static_cast<int>(i);
                if (w->thread->wait_node.linked()) {
                    w->list.remove(w->thread);
                    sched::wake(w->thread);
                }
                woken++;
                break;
            }
        }
    }
    return woken;
}

} // namespace

long waitv(const WaitvRequest* req, u32 n, u64 deadline_ns) {
    if (n == 0 || n > 128)
        return -err::INVAL;
    auto* entries = static_cast<WaitvEntry*>(kmalloc(n * sizeof(WaitvEntry)));
    if (!entries)
        return -err::NOMEM;
    for (u32 i = 0; i < n; i++) {
        if (long r = make_key(req[i].uaddr, req[i].is_private, &entries[i].key); r < 0) {
            kfree(entries);
            return r;
        }
        entries[i].uaddr = req[i].uaddr;
        entries[i].expected = req[i].expected;
    }
    Waitv w;
    w.thread = sched::current();
    w.entries = entries;
    w.count = n;
    long result;
    g_waitv_lock.lock();
    for (u32 i = 0; i < n; i++) {
        if (user_word(entries[i].uaddr) != entries[i].expected) {
            g_waitv_lock.unlock();
            kfree(entries);
            return -err::AGAIN;
        }
    }
    w.next = g_waitv;
    g_waitv = &w;
    __atomic_add_fetch(&g_waitv_count, 1, __ATOMIC_ACQ_REL);
    for (;;) {
        if (w.woken >= 0) {
            result = w.woken;
            break;
        }
        if (signal::interrupted()) {
            result = -err::INTR;
            break;
        }
        if (deadline_ns && clock::now_ns() >= deadline_ns) {
            result = -err::TIMEDOUT;
            break;
        }
        u64 slice = clock::now_ns() + RECHECK_NS;
        sched::block_on(w.list, g_waitv_lock, deadline_ns && deadline_ns < slice ? deadline_ns : slice,
                        [](void* p) { return static_cast<Waitv*>(p)->woken >= 0 || signal::interrupted(); }, &w);
        if (w.woken < 0) {
            // Same safety net as wait(): a word that changed with no wakeup.
            for (u32 i = 0; i < n && w.woken < 0; i++)
                if (user_word(entries[i].uaddr) != entries[i].expected)
                    w.woken = static_cast<int>(i);
        }
    }
    for (Waitv** link = &g_waitv; *link; link = &(*link)->next) {
        if (*link == &w) {
            *link = w.next;
            break;
        }
    }
    __atomic_sub_fetch(&g_waitv_count, 1, __ATOMIC_ACQ_REL);
    g_waitv_lock.unlock();
    kfree(entries);
    return result;
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
    if (woken < count && __atomic_load_n(&g_waitv_count, __ATOMIC_ACQUIRE)) {
        LockGuard wguard(g_waitv_lock);
        woken += wake_waitv(key, count - woken);
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
        if (user_word(uaddr) != expected)
            return -err::AGAIN;
    }
    long total = static_cast<long>(wake_count) + static_cast<long>(requeue_count);
    return wake(uaddr, total > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(total), ~0u, is_private);
}

} // namespace futex
