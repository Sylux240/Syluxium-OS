#include <arch/x86_64/percpu.hpp>
#include <core/log.hpp>
#include <core/panic.hpp>
#include <mm/pmm.hpp>
#include <sched/sched.hpp>
#include <sched/sync.hpp>
#include <tests/selftest.hpp>
#include <time/clock.hpp>

namespace selftest {

namespace {

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond))                                                               \
            panic("self-test failed: %s\n  at %s:%d", #cond, __FILE__, __LINE__);  \
    } while (0)

u32 cpus_online() { return percpu::online_count(); }

// ---- spawn / exit codes / join ----

void exit_with_arg(void* arg) { sched::exit(static_cast<int>(reinterpret_cast<uptr>(arg))); }

void test_spawn_join() {
    constexpr int N = 32;
    sched::Thread* threads[N];
    for (int i = 0; i < N; i++)
        threads[i] = sched::spawn("exit-code", exit_with_arg, reinterpret_cast<void*>(uptr(i * 3)));
    int sum = 0;
    for (int i = 0; i < N; i++)
        sum += sched::join(threads[i]);
    CHECK(sum == 3 * N * (N - 1) / 2);
    klog::ok("  spawn/join: %d threads, exit codes collected", N);
}

// ---- mutex under SMP contention ----

struct MutexTest {
    sched::Mutex mutex;
    u64 counter = 0;
    u64 cpu_mask = 0;
};

constexpr int MUTEX_THREADS = 8;
constexpr int MUTEX_ITERATIONS = 20000;

void mutex_worker(void* arg) {
    auto* t = static_cast<MutexTest*>(arg);
    for (int i = 0; i < MUTEX_ITERATIONS; i++) {
        t->mutex.lock();
        u64 v = t->counter; // deliberately non-atomic read-modify-write
        if ((i & 63) == 0)
            sched::yield(); // invite other threads to break mutual exclusion (they mustn't)
        t->counter = v + 1;
        t->mutex.unlock();
        if ((i & 1023) == 0)
            __atomic_or_fetch(&t->cpu_mask, 1ull << this_cpu()->index, __ATOMIC_RELAXED);
    }
}

void test_mutex() {
    auto* t = new MutexTest;
    sched::Thread* threads[MUTEX_THREADS];
    for (auto& th : threads)
        th = sched::spawn("mutex-worker", mutex_worker, t);
    for (auto* th : threads)
        sched::join(th);
    CHECK(t->counter == u64(MUTEX_THREADS) * MUTEX_ITERATIONS);
    int cpus_used = __builtin_popcountll(t->cpu_mask);
    if (cpus_online() > 1)
        CHECK(cpus_used >= 2);
    klog::ok("  mutex: %d threads x %d locked increments, exact; ran on %d CPU(s)",
             MUTEX_THREADS, MUTEX_ITERATIONS, cpus_used);
    delete t;
}

// ---- preemption: two busy threads on one CPU must take turns ----

struct PreemptTest {
    volatile u64 deadline;
    volatile int last_runner = -1;
    u64 handoffs = 0;
    u64 spins[2] = {};
};

struct PreemptArg {
    PreemptTest* test;
    int id;
};

void busy_worker(void* arg) {
    auto* a = static_cast<PreemptArg*>(arg);
    PreemptTest* t = a->test;
    while (clock::now_ns() < t->deadline) {
        // Never yields: only the timer interrupt can switch us out.
        if (t->last_runner != a->id) {
            t->last_runner = a->id;
            __atomic_add_fetch(&t->handoffs, 1, __ATOMIC_RELAXED);
        }
        t->spins[a->id]++;
    }
}

void test_preemption() {
    auto* t = new PreemptTest;
    t->deadline = clock::now_ns() + 300'000'000; // 300 ms
    int cpu = cpus_online() > 1 ? 1 : 0;         // keep the test thread's CPU free
    PreemptArg args[2] = {{t, 0}, {t, 1}};
    sched::Thread* a = sched::spawn("busy-a", busy_worker, &args[0], cpu);
    sched::Thread* b = sched::spawn("busy-b", busy_worker, &args[1], cpu);
    sched::join(a);
    sched::join(b);
    CHECK(t->spins[0] > 0 && t->spins[1] > 0);
    CHECK(t->handoffs >= 6); // 5 ms slices over 300 ms: expect dozens
    klog::ok("  preemption: 2 non-yielding threads on CPU %d switched %lu times in 300 ms",
             cpu, t->handoffs);
    delete t;
}

// ---- sleeping and timeouts ----

void test_sleep() {
    u64 start = clock::now_ns();
    sched::sleep_ms(50);
    u64 elapsed_ms = (clock::now_ns() - start) / 1'000'000;
    CHECK(elapsed_ms >= 50 && elapsed_ms < 500);

    sched::WaitQueue wq;
    bool never = false;
    start = clock::now_ns();
    bool got = wq.wait_until_timeout([&] { return never; }, 20'000'000);
    u64 waited_ms = (clock::now_ns() - start) / 1'000'000;
    CHECK(!got && waited_ms >= 20 && waited_ms < 500);

    klog::ok("  sleep: 50 ms sleep took %lu ms; 20 ms wait timeout took %lu ms", elapsed_ms, waited_ms);
}

// ---- ping-pong between two threads ----

struct PingPong {
    sched::Semaphore ping{0};
    sched::Semaphore pong{0};
    int rounds = 0;
};

constexpr int PING_ROUNDS = 2000;

void ponger(void* arg) {
    auto* p = static_cast<PingPong*>(arg);
    for (int i = 0; i < PING_ROUNDS; i++) {
        p->ping.wait();
        p->rounds++;
        p->pong.signal();
    }
}

void test_ping_pong() {
    auto* p = new PingPong;
    sched::Thread* t = sched::spawn("ponger", ponger, p);
    u64 start = clock::now_ns();
    for (int i = 0; i < PING_ROUNDS; i++) {
        p->ping.signal();
        p->pong.wait();
    }
    u64 us = (clock::now_ns() - start) / 1000;
    sched::join(t);
    CHECK(p->rounds == PING_ROUNDS);
    klog::ok("  semaphores: %d ping-pong round trips in %lu us", PING_ROUNDS, us);
    delete p;
}

// ---- lots of short-lived detached threads ----

struct Swarm {
    u64 done = 0;
    sched::Completion all_done;
};

constexpr int SWARM_SIZE = 300;

void swarm_worker(void* arg) {
    auto* s = static_cast<Swarm*>(arg);
    if (__atomic_add_fetch(&s->done, 1, __ATOMIC_ACQ_REL) == SWARM_SIZE)
        s->all_done.complete();
}

void test_swarm() {
    u64 threads_before = sched::thread_count();
    auto* s = new Swarm;
    for (int i = 0; i < SWARM_SIZE; i++)
        sched::spawn("swarm", swarm_worker, s, -1, true);
    CHECK(s->all_done.wait_timeout(5'000'000'000));

    // The reaper frees exited threads in the background.
    u64 deadline = clock::now_ns() + 2'000'000'000;
    while (sched::thread_count() > threads_before && clock::now_ns() < deadline)
        sched::sleep_ms(5);
    CHECK(sched::thread_count() == threads_before);
    klog::ok("  swarm: %d detached threads ran and were reaped", SWARM_SIZE);
    delete s;
}

} // namespace

void run_scheduler_tests() {
    klog::info("Running scheduler self-tests...");
    u64 free_before = pmm::stats().free_pages;

    test_spawn_join();
    test_mutex();
    test_preemption();
    test_sleep();
    test_ping_pong();
    test_swarm();

    // Exited threads' stacks and structs must all be back (small slack for
    // slab caches and page tables created along the way).
    u64 free_after = pmm::stats().free_pages;
    CHECK(free_after + 64 >= free_before);
    klog::ok("All scheduler self-tests passed (memory back to within %ld pages)",
             static_cast<i64>(free_before) - static_cast<i64>(free_after));
}

} // namespace selftest
