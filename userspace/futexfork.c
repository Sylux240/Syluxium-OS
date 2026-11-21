// Futex wakeups while the process forks -- what Firefox does (it forks its
// content processes while dozens of threads hand work to each other).
// After fork() every private page is shared copy-on-write, and the next
// store to a futex word moves it to a new physical page: wakeups must
// still reach the threads asleep on it.
//
//   futexfork [seconds]
//
//  1. shared:  non-private FUTEX_WAIT / FUTEX_WAKE ping-pong on heap words
//              (Mesa's way) while another thread forks continuously.
//  2. join:    pthread_create + pthread_join (the kernel's CLONE_CHILD_CLEARTID
//              wake, a non-private futex) while forking.
//  3. wakeop:  FUTEX_WAKE_OP: its arithmetic, comparison and both wakes.
//  4. condvar: pthread_cond_broadcast / signal storms and barriers, 8 threads,
//              while forking.
//  5. pi:      a PTHREAD_PRIO_INHERIT mutex under contention (FUTEX_LOCK_PI).
//  6. robust:  a robust mutex (plain and PI) whose owner exits holding it:
//              the waiter gets EOWNERDEAD (the kernel's robust-list walk).
// A handoff that takes over 500 ms is a lost wakeup (the kernel's safety
// net ends such a wait after a second); any is a failure.
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
static void check(int ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        fails++;
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static long futex(void* addr, int op, uint32_t val, void* ts_or_val2, void* addr2, uint32_t val3) {
    return syscall(SYS_futex, addr, op, val, ts_or_val2, addr2, val3);
}

static atomic_int stop_forking;
static atomic_long forks;
static void* forker(void* a) {
    (void)a;
    while (!atomic_load(&stop_forking)) {
        pid_t c = fork();
        if (c == 0)
            _exit(0);
        if (c > 0)
            waitpid(c, NULL, 0);
        atomic_fetch_add(&forks, 1);
        // Touch lots of pages so they get copied back (as a busy parent would).
        static char scratch[64 * 4096];
        for (size_t i = 0; i < sizeof(scratch); i += 4096)
            scratch[i]++;
    }
    return NULL;
}

static pthread_t start_forker(void) {
    pthread_t t;
    atomic_store(&stop_forking, 0);
    pthread_create(&t, NULL, forker, NULL);
    return t;
}

static void stop_forker(pthread_t t) {
    atomic_store(&stop_forking, 1);
    pthread_join(t, NULL);
}

// ---- 1. shared (non-private) futex ping-pong ----
#define PAIRS 4
struct Pair {
    _Atomic uint32_t* word; // on the heap: private memory
    double end;
    long rounds;
    double worst;
};

#define STOP 0xFFFFFFFFu
// Wait until the word is v (or STOP).
static void wait_for(_Atomic uint32_t* w, uint32_t v, double* worst) {
    double t0 = now();
    for (;;) {
        uint32_t cur = atomic_load(w);
        if (cur == v || cur == STOP)
            break;
        futex((void*)w, FUTEX_WAIT, cur, NULL, NULL, 0);
    }
    double d = now() - t0;
    if (d > *worst)
        *worst = d;
}

static void* pong(void* arg) {
    struct Pair* p = arg;
    for (uint32_t i = 1;; i += 2) {
        wait_for(p->word, i, &p->worst);
        if (atomic_load(p->word) == STOP)
            break;
        atomic_store(p->word, i + 1);
        futex((void*)p->word, FUTEX_WAKE, 1, NULL, NULL, 0);
    }
    return NULL;
}

static void test_shared(int secs) {
    pthread_t f = start_forker();
    struct Pair pairs[PAIRS];
    pthread_t th[PAIRS];
    double end = now() + secs;
    for (int i = 0; i < PAIRS; i++) {
        pairs[i].word = malloc(64);
        atomic_store(pairs[i].word, 0);
        pairs[i].rounds = 0;
        pairs[i].worst = 0;
        pthread_create(&th[i], NULL, pong, &pairs[i]);
    }
    double worst = 0;
    // Drive every pair from here, round-robin.
    uint32_t v = 0;
    while (now() < end) {
        for (int i = 0; i < PAIRS; i++) {
            atomic_store(pairs[i].word, v + 1);
            futex((void*)pairs[i].word, FUTEX_WAKE, 1, NULL, NULL, 0);
        }
        for (int i = 0; i < PAIRS; i++) {
            wait_for(pairs[i].word, v + 2, &worst);
            pairs[i].rounds++;
        }
        v += 2;
    }
    for (int i = 0; i < PAIRS; i++) { // let them finish
        atomic_store(pairs[i].word, STOP);
        futex((void*)pairs[i].word, FUTEX_WAKE, 1, NULL, NULL, 0);
        pthread_join(th[i], NULL);
        if (pairs[i].worst > worst)
            worst = pairs[i].worst;
    }
    stop_forker(f);
    char msg[160];
    snprintf(msg, sizeof(msg), "non-private futex ping-pong while forking: %ld rounds, %ld forks, worst handoff %.0f ms",
             pairs[0].rounds, atomic_load(&forks), worst * 1000);
    check(worst < 0.5 && pairs[0].rounds > 10, msg);
}

// ---- 2. pthread_join while forking ----
static void* nothing(void* a) { return a; }

static void test_join(int secs) {
    pthread_t f = start_forker();
    double end = now() + secs, worst = 0;
    long joins = 0;
    while (now() < end) {
        pthread_t t;
        pthread_create(&t, NULL, nothing, NULL);
        double t0 = now();
        pthread_join(t, NULL);
        double d = now() - t0;
        if (d > worst)
            worst = d;
        joins++;
    }
    stop_forker(f);
    char msg[128];
    snprintf(msg, sizeof(msg), "pthread_join while forking: %ld joins, worst %.0f ms", joins, worst * 1000);
    check(worst < 0.5 && joins > 10, msg);
}

// ---- 3. FUTEX_WAKE_OP ----
static _Atomic uint32_t w1, w2;
static atomic_int woke1, woke2;
static void* sleep_on1(void* a) {
    (void)a;
    while (atomic_load(&w1) == 0)
        futex((void*)&w1, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
    atomic_fetch_add(&woke1, 1);
    return NULL;
}
static void* sleep_on2(void* a) {
    (void)a;
    uint32_t start = atomic_load(&w2);
    while (atomic_load(&w2) == start)
        futex((void*)&w2, FUTEX_WAIT_PRIVATE, start, NULL, NULL, 0);
    atomic_fetch_add(&woke2, 1);
    return NULL;
}

static void test_wakeop(void) {
    // Arithmetic: ADD 5 to w2 when it is 10; compare old value == 10.
    atomic_store(&w2, 10);
    long r = futex((void*)&w1, FUTEX_WAKE_OP_PRIVATE, 0, (void*)0, (void*)&w2,
                   FUTEX_OP(FUTEX_OP_ADD, 5, FUTEX_OP_CMP_EQ, 10));
    int ok = r == 0 && atomic_load(&w2) == 15;
    atomic_store(&w2, 0xF0);
    futex((void*)&w1, FUTEX_WAKE_OP_PRIVATE, 0, 0, (void*)&w2, FUTEX_OP(FUTEX_OP_ANDN, 0x30, FUTEX_OP_CMP_GT, 0));
    ok = ok && atomic_load(&w2) == 0xC0;
    futex((void*)&w1, FUTEX_WAKE_OP_PRIVATE, 0, 0, (void*)&w2, FUTEX_OP(FUTEX_OP_OR, 4, FUTEX_OP_CMP_GT, 0) | (8u << 28));
    ok = ok && atomic_load(&w2) == 0xD0; // OPARG_SHIFT: 1 << 4
    futex((void*)&w1, FUTEX_WAKE_OP_PRIVATE, 0, 0, (void*)&w2, FUTEX_OP(FUTEX_OP_SET, -1, FUTEX_OP_CMP_EQ, 0));
    ok = ok && atomic_load(&w2) == 0xFFFFFFFF; // signed 12-bit argument
    check(ok, "FUTEX_WAKE_OP arithmetic (ADD, ANDN, OR with shift, SET of a negative)");

    // Wakes: 2 sleepers on each word; wake 1 on w1 and (comparison true) 2 on w2.
    atomic_store(&w1, 0);
    atomic_store(&w2, 0);
    atomic_store(&woke1, 0);
    atomic_store(&woke2, 0);
    pthread_t t[4];
    pthread_create(&t[0], NULL, sleep_on1, NULL);
    pthread_create(&t[1], NULL, sleep_on1, NULL);
    pthread_create(&t[2], NULL, sleep_on2, NULL);
    pthread_create(&t[3], NULL, sleep_on2, NULL);
    usleep(200000); // all four asleep
    atomic_store(&w1, 1);
    r = futex((void*)&w1, FUTEX_WAKE_OP_PRIVATE, 1, (void*)2, (void*)&w2, FUTEX_OP(FUTEX_OP_ADD, 1, FUTEX_OP_CMP_EQ, 0));
    usleep(200000);
    int one_on_w1 = atomic_load(&woke1) == 1, both_on_w2 = atomic_load(&woke2) == 2;
    char msg[128];
    snprintf(msg, sizeof(msg), "FUTEX_WAKE_OP wakes 1 + 2 (returned %ld, woke %d + %d)", r, atomic_load(&woke1),
             atomic_load(&woke2));
    check(r == 3 && one_on_w1 && both_on_w2, msg);
    futex((void*)&w1, FUTEX_WAKE_PRIVATE, 10, NULL, NULL, 0);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    // A false comparison wakes nobody on the second word.
    atomic_store(&w2, 7);
    atomic_store(&woke2, 0);
    pthread_create(&t[2], NULL, sleep_on2, NULL);
    usleep(100000);
    r = futex((void*)&w1, FUTEX_WAKE_OP_PRIVATE, 1, (void*)1, (void*)&w2, FUTEX_OP(FUTEX_OP_SET, 7, FUTEX_OP_CMP_NE, 7));
    usleep(100000);
    check(r == 0 && atomic_load(&woke2) == 0, "FUTEX_WAKE_OP: false comparison, no second wake");
    atomic_store(&w2, 8);
    futex((void*)&w2, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    pthread_join(t[2], NULL);
}

// ---- 4. condvar / barrier storms while forking ----
#define CV_THREADS 8
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_barrier_t barrier;
static long generation;
static atomic_int cv_stop;
static double cv_worst;

static void* cv_worker(void* a) {
    (void)a;
    long seen = 0;
    for (;;) {
        double t0 = now();
        pthread_mutex_lock(&m);
        while (generation == seen && !atomic_load(&cv_stop))
            pthread_cond_wait(&cv, &m);
        if (generation == seen) { // stopped: main won't come to the barrier again
            pthread_mutex_unlock(&m);
            break;
        }
        seen = generation;
        pthread_mutex_unlock(&m);
        double d = now() - t0;
        if (d > cv_worst)
            cv_worst = d;
        t0 = now();
        pthread_barrier_wait(&barrier);
        d = now() - t0;
        if (d > cv_worst)
            cv_worst = d;
    }
    return NULL;
}

static void test_condvar(int secs) {
    pthread_t f = start_forker();
    pthread_barrier_init(&barrier, NULL, CV_THREADS + 1);
    pthread_t t[CV_THREADS];
    atomic_store(&cv_stop, 0);
    for (int i = 0; i < CV_THREADS; i++)
        pthread_create(&t[i], NULL, cv_worker, NULL);
    double end = now() + secs, worst_main = 0;
    long rounds = 0;
    while (now() < end) {
        pthread_mutex_lock(&m);
        generation++;
        if (rounds & 1) {
            pthread_cond_broadcast(&cv);
        } else {
            for (int i = 0; i < CV_THREADS; i++)
                pthread_cond_signal(&cv);
        }
        pthread_mutex_unlock(&m);
        double t0 = now();
        pthread_barrier_wait(&barrier);
        double d = now() - t0;
        if (d > worst_main)
            worst_main = d;
        rounds++;
    }
    pthread_mutex_lock(&m);
    atomic_store(&cv_stop, 1);
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&m);
    for (int i = 0; i < CV_THREADS; i++)
        pthread_join(t[i], NULL);
    stop_forker(f);
    double worst = cv_worst > worst_main ? cv_worst : worst_main;
    char msg[128];
    snprintf(msg, sizeof(msg), "condvar signal/broadcast + barrier storm while forking: %ld rounds, worst %.0f ms",
             rounds, worst * 1000);
    check(worst < 0.5 && rounds > 10, msg);
}

// ---- 5. priority-inheritance mutex (FUTEX_LOCK_PI / UNLOCK_PI) ----
#define PI_THREADS 6
#define PI_ROUNDS 20000
static pthread_mutex_t pi_m;
static long pi_count;
static double pi_worst;
static void* pi_worker(void* a) {
    (void)a;
    for (int i = 0; i < PI_ROUNDS; i++) {
        double t0 = now();
        pthread_mutex_lock(&pi_m);
        double d = now() - t0;
        if (d > pi_worst)
            pi_worst = d;
        pi_count++;
        pthread_mutex_unlock(&pi_m);
    }
    return NULL;
}

static void test_pi(void) {
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    int r = pthread_mutexattr_setprotocol(&at, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&pi_m, &at);
    pthread_t t[PI_THREADS];
    for (int i = 0; i < PI_THREADS; i++)
        pthread_create(&t[i], NULL, pi_worker, NULL);
    for (int i = 0; i < PI_THREADS; i++)
        pthread_join(t[i], NULL);
    char msg[128];
    snprintf(msg, sizeof(msg), "PRIO_INHERIT mutex, %d threads: count %ld of %d, worst wait %.0f ms", PI_THREADS,
             pi_count, PI_THREADS * PI_ROUNDS, pi_worst * 1000);
    check(r == 0 && pi_count == PI_THREADS * PI_ROUNDS && pi_worst < 0.5, msg);
    pthread_mutex_lock(&pi_m);
    check(pthread_mutex_trylock(&pi_m) == EBUSY, "PI trylock of a held mutex: EBUSY");
    pthread_mutex_unlock(&pi_m);
}

// ---- 6. robust mutex whose owner dies ----
static pthread_mutex_t rb_m;
static atomic_int rb_locked;
static void* rb_owner(void* a) {
    pthread_mutex_lock(&rb_m);
    atomic_store(&rb_locked, 1);
    usleep((useconds_t)(long)a);
    return NULL; // exits holding it
}

static void test_robust(int pi) {
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_setrobust(&at, PTHREAD_MUTEX_ROBUST);
    if (pi)
        pthread_mutexattr_setprotocol(&at, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&rb_m, &at);
    atomic_store(&rb_locked, 0);
    pthread_t t;
    pthread_create(&t, NULL, rb_owner, (void*)200000L); // holds it 200 ms, we block meanwhile
    while (!atomic_load(&rb_locked))
        usleep(1000);
    double t0 = now();
    int r = pthread_mutex_lock(&rb_m);
    double d = now() - t0;
    int ok = r == EOWNERDEAD && pthread_mutex_consistent(&rb_m) == 0 && pthread_mutex_unlock(&rb_m) == 0;
    ok = ok && pthread_mutex_lock(&rb_m) == 0 && pthread_mutex_unlock(&rb_m) == 0;
    pthread_join(t, NULL);
    char msg[128];
    snprintf(msg, sizeof(msg), "robust%s mutex: owner exits while we wait -> EOWNERDEAD after %.0f ms (got %d)",
             pi ? " PI" : "", d * 1000, r);
    check(ok && d < 0.7, msg);
}

int main(int argc, char** argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 3;
    setvbuf(stdout, NULL, _IONBF, 0);
    test_shared(secs);
    test_join(secs);
    test_wakeop();
    test_condvar(secs);
    test_pi();
    test_robust(0);
    test_robust(1);
    printf("futexfork: %d failed\n", fails);
    return fails;
}
