#include <core/panic.hpp>
#include <arch/x86_64/interrupts.hpp>
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

// Waiters hash into buckets by key. Futexes in memory shared between
// processes (MAP_SHARED) are keyed by physical address, so the processes
// meet at the same futex; all others by (address space, virtual address) --
// also "shared" (non-PRIVATE) operations on private memory, as on Linux:
// such a page can be copied on write (after fork) between a waiter's
// FUTEX_WAIT and the waker's store, and a physical key would then differ
// and lose the wakeup. (Mesa's threads use non-private futexes this way.)
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
    if (is_private || !p->mem->shared_at(uaddr)) {
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

} // namespace
} // namespace futex

// futex_access.S: accesses that report a page fault instead of resolving it.
extern "C" int futex_try_read(const u32* uaddr, u32* out);
extern "C" int futex_try_cmpxchg(u32* uaddr, u32* expected, u32 desired);
extern "C" char futex_access_insn_read[], futex_access_insn_cmpxchg[], futex_access_fixup[];

namespace futex {

bool fault_fixup(interrupts::Frame* frame) {
    if (frame->rip != reinterpret_cast<u64>(futex_access_insn_read) &&
        frame->rip != reinterpret_cast<u64>(futex_access_insn_cmpxchg))
        return false;
    frame->rip = reinterpret_cast<u64>(futex_access_fixup);
    return true;
}

namespace {

// The futex word, read while holding a spinlock: false = the page isn't
// there (or is being changed); drop the lock, fault it in, try again. A
// page fault under a spinlock must never be resolved: that can sleep.
bool try_read(u64 uaddr, u32* out) {
    protect::UserAccess window;
    return futex_try_read(reinterpret_cast<const u32*>(uaddr), out) == 0;
}

// Compare-and-swap of the word under a spinlock: 1 swapped, 0 it held
// another value (now in *expected), -1 page fault (as for try_read).
int try_cas(u64 uaddr, u32* expected, u32 desired) {
    protect::UserAccess window;
    int r = futex_try_cmpxchg(reinterpret_cast<u32*>(uaddr), expected, desired);
    return r < 0 ? -1 : r == 0 ? 1 : 0;
}

// Fault the word in (no spinlock held). 0 or -EFAULT.
long fault_in(u64 uaddr, bool write) { return uaccess::check(reinterpret_cast<void*>(uaddr), 4, write); }

// Read with no lock held, faulting the page in as needed.
long read_unlocked(u64 uaddr, u32* out) {
    for (int attempt = 0; attempt < 8; attempt++) {
        if (try_read(uaddr, out))
            return 0;
        if (long r = fault_in(uaddr, false); r < 0)
            return r;
    }
    return -err::FAULT;
}

bool matches(const sched::Thread* t, const Key& k) { return t->futex_key_a == k.a && t->futex_key_b == k.b; }

// Wake generations per key: every wake() of a key bumps its slot. A waiter
// whose word changed while it slept was only *missed* if no wake of its key
// happened meanwhile -- FUTEX_WAKE(1) on a word many threads sleep on (glibc's
// condition variables) rightly leaves the others asleep on a changed word.
// Direct-mapped by key hash; a slot taken over by another key means "can't
// tell" (the waiter then just returns, which is always allowed).
struct WakeGen {
    u64 key_a, key_b;
    u64 gen;
};
constexpr u32 WAKE_GENS = 4096;
WakeGen g_wake_gens[WAKE_GENS];

WakeGen& gen_slot(const Key& k) {
    u64 h = (k.a * 0xD6E8FEB86659FD93ull) ^ (k.b * 0x9E3779B97F4A7C15ull);
    return g_wake_gens[(h >> 40) % WAKE_GENS];
}

// Under the key's bucket lock: make the slot count this key's wakes.
void claim_gen(const Key& k) {
    WakeGen& g = gen_slot(k);
    if (g.key_a != k.a || g.key_b != k.b) {
        g.key_a = k.a;
        g.key_b = k.b;
    }
}

// Under the key's bucket lock: a wake of this key.
void note_wake(const Key& k) {
    WakeGen& g = gen_slot(k);
    claim_gen(k);
    __atomic_add_fetch(&g.gen, 1, __ATOMIC_RELAXED);
}

// The key's generation now, or ~0 if its slot belongs to another key.
u64 wake_gen(const Key& k) {
    WakeGen& g = gen_slot(k);
    return g.key_a == k.a && g.key_b == k.b ? __atomic_load_n(&g.gen, __ATOMIC_RELAXED) : ~0ull;
}

// Where in the program a waiter sleeps: its futex call's address and the
// code addresses on its stack, as file inode + offset (match the inode
// number to a library with debugfs "ncheck" on the root file system).
void report_waiter_code() {
    sched::Thread* t = sched::current();
    proc::Process* p = proc::current();
    if (!t->syscall_frame || !p || !p->mem)
        return;
    struct Ctx {
        u64 addr;
        u64 ino, off;
        bool exec;
    };
    auto where = [&](u64 addr, Ctx* c) {
        c->addr = addr;
        c->ino = 0;
        c->exec = false;
        p->mem->for_each_vma([](const proc::Vma& v, void* a) {
            auto* c = static_cast<Ctx*>(a);
            if (c->addr >= v.start && c->addr < v.end && v.file) {
                c->ino = v.file->ino;
                c->off = v.file_offset + (c->addr - v.start);
                c->exec = v.prot & proc::PROT_EXEC;
            }
        }, c);
    };
    Ctx c;
    where(t->syscall_frame->rip, &c);
    klog::info("  waiting at ip %#lx = ino %lu +%#lx", t->syscall_frame->rip, c.ino, c.ino ? c.off : 0);
    u64 stack[64];
    usize got = p->mem->read_remote(t->syscall_frame->rsp, stack, sizeof(stack));
    int shown = 0;
    for (usize i = 0; i < got / 8 && shown < 10; i++) {
        where(stack[i], &c);
        if (c.ino && c.exec) {
            klog::info("    stack+%#lx: %#lx = ino %lu +%#lx", i * 8, stack[i], c.ino, c.off);
            shown++;
        }
    }
}


} // namespace

long wait(u64 uaddr, u32 expected, u64 deadline_ns, u32 bitset, bool is_private) {
    if (bitset == 0)
        return -err::INVAL;
    Key key;
    if (long r = make_key(uaddr, is_private, &key); r < 0)
        return r;
    Bucket& b = bucket_for(key);
    sched::Thread* me = sched::current();

    // The word is checked under the bucket lock, so a waker that changes it
    // and then calls wake() can't slip in between the check and the sleep.
    for (;;) {
        b.lock.lock();
        u32 current;
        if (try_read(uaddr, &current)) {
            if (current != expected) {
                b.lock.unlock();
                return -err::AGAIN;
            }
            break;
        }
        b.lock.unlock();
        if (long r = fault_in(uaddr, false); r < 0)
            return r;
    }
    if (signal::interrupted()) {
        b.lock.unlock();
        return -err::INTR;
    }
    me->futex_key_a = key.a;
    me->futex_key_b = key.b;
    me->futex_bitset = bitset;
    claim_gen(key); // its wakes from now on are counted
    u64 gen_at_sleep = wake_gen(key);
    bool missed = false;
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
        u32 now_val;
        if (!try_read(uaddr, &now_val)) {
            woken = true; // can't look right now: a spurious return is always allowed
            break;
        }
        if (now_val != expected) {
            u64 gen = wake_gen(key);
            if (gen != ~0ull && gen != gen_at_sleep)
                continue; // woken others: we stay asleep, as on Linux
            if (gen == gen_at_sleep) { // the word changed and nobody woke this key at all
                g_lost_wakeups++;
                missed = g_lost_wakeups <= 20;
                if (missed)
                    klog::warn("futex: lost wakeup caught (tid %d, word %#lx: %u -> %u, %s)", me->user_tid, uaddr,
                               expected, now_val, is_private ? "private" : "shared");
            }
            woken = true; // (or can't tell: returning is always allowed)
            break;
        }
    }
    me->futex_key_a = me->futex_key_b = 0;
    b.lock.unlock();
    if (missed && g_lost_wakeups <= 3)
        report_waiter_code(); // (reads the address space: not under the bucket lock)

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
    for (u32 i = 0;;) {
        if (i == 0)
            g_waitv_lock.lock();
        if (i == n)
            break;
        u32 v;
        if (!try_read(entries[i].uaddr, &v)) {
            g_waitv_lock.unlock();
            if (long r = fault_in(entries[i].uaddr, false); r < 0) {
                kfree(entries);
                return r;
            }
            i = 0; // look at them all again, under the lock
            continue;
        }
        if (v != entries[i].expected) {
            g_waitv_lock.unlock();
            kfree(entries);
            return -err::AGAIN;
        }
        i++;
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
            for (u32 i = 0; i < n && w.woken < 0; i++) {
                u32 v;
                if (!try_read(entries[i].uaddr, &v) || v != entries[i].expected)
                    w.woken = static_cast<int>(i);
            }
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
            if (t->state != sched::ThreadState::Blocked && t->state != sched::ThreadState::Ready &&
                t->state != sched::ThreadState::Running)
                panic("futex: wake: waiter %p in the bucket is not a live thread (state %d, tid %lu)", t,
                      static_cast<int>(t->state), t->tid);
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
    note_wake(key);
    return woken;
}

// ---- PI futexes and robust lists ----
namespace {

constexpr u32 FUTEX_WAITERS = 0x80000000u, FUTEX_OWNER_DIED = 0x40000000u, FUTEX_TID_MASK = 0x3fffffffu;


// Under the bucket lock: the first waiter of `key` other than `except` (null: none).
sched::Thread* first_waiter(Bucket& b, const Key& key, const sched::Thread* except) {
    for (sched::Thread* t = b.waiters.front(); t; t = b.waiters.next_of(t))
        if (t != except && matches(t, key))
            return t;
    return nullptr;
}

// A PI wait isn't ended by ordinary signals (glibc takes any LOCK_PI result
// but ESRCH/EDEADLK as "locked"): only by the process dying.
bool dying(void*) {
    proc::Process* p = proc::current();
    u64 kill = 1ull << (signal::SIGKILL - 1);
    return p->group_exiting || ((__atomic_load_n(&p->sig_pending, __ATOMIC_ACQUIRE) |
                                 __atomic_load_n(&sched::current()->sig_pending, __ATOMIC_ACQUIRE)) & kill);
}

} // namespace

long lock_pi(u64 uaddr, u64 deadline_ns, bool try_only, bool is_private) {
    if (uaddr & 3)
        return -err::INVAL;
    if (long r = uaccess::check(reinterpret_cast<void*>(uaddr), 4, true); r < 0)
        return r;
    Key key;
    if (long r = make_key(uaddr, is_private, &key); r < 0)
        return r;
    Bucket& b = bucket_for(key);
    sched::Thread* me = sched::current();
    u32 tid = static_cast<u32>(me->vtid) & FUTEX_TID_MASK;
    bool slept = false;
    long result;
    b.lock.lock();
    // A page fault on the word: let go of the bucket, fault it in, go on.
    auto refault = [&]() {
        b.lock.unlock();
        long r = fault_in(uaddr, true);
        b.lock.lock();
        return r == 0;
    };
    for (;;) {
        u32 v;
        if (!try_read(uaddr, &v)) {
            if (!refault()) {
                result = -err::FAULT;
                break;
            }
            continue;
        }
        if ((v & FUTEX_TID_MASK) == tid) {
            result = slept ? 0 : -err::DEADLK; // handed to us by unlock_pi, or a relock
            break;
        }
        if ((v & FUTEX_TID_MASK) == 0) {
            u32 want = tid | (v & FUTEX_OWNER_DIED) | (first_waiter(b, key, me) ? FUTEX_WAITERS : 0);
            int c = try_cas(uaddr, &v, want);
            if (c == 1) {
                result = 0;
                break;
            }
            if (c < 0 && !refault()) {
                result = -err::FAULT;
                break;
            }
            continue;
        }
        if (try_only) {
            result = -err::AGAIN;
            break;
        }
        if (!(v & FUTEX_WAITERS)) {
            int c = try_cas(uaddr, &v, v | FUTEX_WAITERS);
            if (c < 0 && !refault()) {
                result = -err::FAULT;
                break;
            }
            if (c != 1)
                continue; // changed under us (or faulted): look again
        }
        if (dying(nullptr)) {
            result = -err::INTR;
            break;
        }
        if (deadline_ns && clock::now_ns() >= deadline_ns) {
            result = -err::TIMEDOUT;
            break;
        }
        me->futex_key_a = key.a;
        me->futex_key_b = key.b;
        me->futex_bitset = ~0u;
        u64 slice = clock::now_ns() + RECHECK_NS;
        sched::block_on(b.waiters, b.lock, deadline_ns && deadline_ns < slice ? deadline_ns : slice, dying, nullptr);
        me->futex_key_a = me->futex_key_b = 0;
        slept = true;
    }
    b.lock.unlock();
    return result;
}

long unlock_pi(u64 uaddr, bool is_private) {
    if (uaddr & 3)
        return -err::INVAL;
    if (long r = uaccess::check(reinterpret_cast<void*>(uaddr), 4, true); r < 0)
        return r;
    Key key;
    if (long r = make_key(uaddr, is_private, &key); r < 0)
        return r;
    Bucket& b = bucket_for(key);
    u32 tid = static_cast<u32>(sched::current()->vtid) & FUTEX_TID_MASK;
    b.lock.lock();
    long result;
    for (;;) {
        u32 v;
        int c;
        if (!try_read(uaddr, &v)) {
            c = -1;
        } else {
            if ((v & FUTEX_TID_MASK) != tid) {
                result = -err::PERM;
                break;
            }
            sched::Thread* next = first_waiter(b, key, nullptr);
            if (!next) {
                c = try_cas(uaddr, &v, 0);
                if (c == 1) {
                    result = 0;
                    break;
                }
            } else {
                // Hand the lock over: the waiter wakes up owning it.
                b.waiters.remove(next);
                u32 want = (static_cast<u32>(next->vtid) & FUTEX_TID_MASK) |
                           (first_waiter(b, key, nullptr) ? FUTEX_WAITERS : 0);
                c = try_cas(uaddr, &v, want);
                if (c == 1) {
                    sched::wake(next);
                    result = 0;
                    break;
                }
                b.waiters.push_front(next);
            }
        }
        if (c < 0) { // page fault: let go of the bucket, fault it in, retry
            b.lock.unlock();
            long r = fault_in(uaddr, true);
            b.lock.lock();
            if (r < 0) {
                result = r;
                break;
            }
        }
    }
    b.lock.unlock();
    return result;
}

void exit_robust_list(u64 head, int tid) {
    struct Head {
        u64 next;
        i64 futex_offset;
        u64 pending;
    } h;
    if (!head || uaccess::copy_from(&h, reinterpret_cast<const void*>(head), sizeof(h)) < 0)
        return;
    auto release = [&](u64 entry) {
        u64 uaddr = entry + static_cast<u64>(h.futex_offset);
        if ((uaddr & 3) || uaccess::check(reinterpret_cast<void*>(uaddr), 4, true) < 0)
            return;
        u32 v;
        if (read_unlocked(uaddr, &v) < 0)
            return;
        for (int attempt = 0; attempt < 64 && (v & FUTEX_TID_MASK) == static_cast<u32>(tid); attempt++) {
            int c = try_cas(uaddr, &v, (v & FUTEX_WAITERS) | FUTEX_OWNER_DIED);
            if (c == 1) {
                if (v & FUTEX_WAITERS)
                    wake(uaddr, 1, ~0u, false); // a PI waiter takes the lock itself in lock_pi
                break;
            }
            if (c < 0 && (fault_in(uaddr, true) < 0 || read_unlocked(uaddr, &v) < 0))
                return;
        }
    };
    // Bit 0 of each pointer tags a PI mutex (glibc); the address is the rest.
    h.next &= ~1ull;
    h.pending &= ~1ull;
    u64 entry = h.next;
    for (int n = 0; entry && entry != head && n < 2048; n++) { // a loop in the list must not hang the exit
        u64 next;
        if (uaccess::get(&next, reinterpret_cast<const u64*>(entry)) < 0)
            break;
        if (entry != h.pending)
            release(entry);
        entry = next & ~1ull;
    }
    if (h.pending)
        release(h.pending);
}

long wake_op(u64 uaddr, int nr_wake, u64 uaddr2, int nr_wake2, u32 val3, bool is_private) {
    constexpr u32 OP_SET = 0, OP_ADD = 1, OP_OR = 2, OP_ANDN = 3, OP_XOR = 4, OP_OPARG_SHIFT = 8;
    constexpr u32 CMP_EQ = 0, CMP_NE = 1, CMP_LT = 2, CMP_LE = 3, CMP_GT = 4, CMP_GE = 5;
    u32 op = (val3 >> 28) & 0xf, cmp = (val3 >> 24) & 0xf;
    // Both arguments are signed 12-bit numbers.
    i32 oparg = static_cast<i32>((val3 >> 12) & 0xfff) << 20 >> 20;
    i32 cmparg = static_cast<i32>(val3 & 0xfff) << 20 >> 20;
    if (op & OP_OPARG_SHIFT) {
        if (oparg < 0 || oparg > 31)
            return -err::INVAL;
        oparg = static_cast<i32>(1u << oparg);
        op &= ~OP_OPARG_SHIFT;
    }
    if (op > OP_XOR || cmp > CMP_GE)
        return -err::NOSYS;
    if (uaddr2 & 3)
        return -err::INVAL;
    // Writable and present (a copy-on-write page gets its own copy now).
    if (long r = uaccess::check(reinterpret_cast<void*>(uaddr2), 4, true); r < 0)
        return r;
    i32 old;
    {
        protect::UserAccess window;
        auto* word = reinterpret_cast<i32*>(uaddr2);
        old = __atomic_load_n(word, __ATOMIC_RELAXED);
        for (;;) {
            i32 next;
            switch (op) {
            case OP_SET: next = oparg; break;
            case OP_ADD: next = static_cast<i32>(static_cast<u32>(old) + static_cast<u32>(oparg)); break;
            case OP_OR: next = old | oparg; break;
            case OP_ANDN: next = old & ~oparg; break;
            default: next = old ^ oparg; break;
            }
            if (__atomic_compare_exchange_n(word, &old, next, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
                break;
        }
    }
    bool pass;
    switch (cmp) {
    case CMP_EQ: pass = old == cmparg; break;
    case CMP_NE: pass = old != cmparg; break;
    case CMP_LT: pass = old < cmparg; break;
    case CMP_LE: pass = old <= cmparg; break;
    case CMP_GT: pass = old > cmparg; break;
    default: pass = old >= cmparg; break;
    }
    long woken = wake(uaddr, nr_wake, ~0u, is_private);
    if (woken < 0)
        return woken;
    if (pass) {
        long w2 = wake(uaddr2, nr_wake2, ~0u, is_private);
        if (w2 > 0)
            woken += w2;
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
        u32 v;
        if (long r = read_unlocked(uaddr, &v); r < 0)
            return r;
        if (v != expected)
            return -err::AGAIN;
    }
    long total = static_cast<long>(wake_count) + static_cast<long>(requeue_count);
    return wake(uaddr, total > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int>(total), ~0u, is_private);
}

} // namespace futex
