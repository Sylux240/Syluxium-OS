// Futex stress test: the patterns that froze a Python GTK program (main
// thread asleep in FUTEX_WAIT, a worker waiting for the GIL, a pipe reader).
//
//   futexstress [seconds]
//
//  1. gil:    CPython's GIL handoff -- a mutex + condition variable with 5 ms
//             timed waits and drop requests, 8 threads, one of them feeding
//             on a pipe that another thread writes in bursts.
//  2. pingpong: pairs of threads handing a token back and forth with
//             condition variables (lots of wait/wake ordering races).
//  4. signal:  a signal sent to a thread about to sleep in FUTEX_WAIT must
//             end the wait at once (EINTR), not be lost.
//  3. timeout: raw FUTEX_WAIT with tiny timeouts racing FUTEX_WAKE, and
//             FUTEX_WAIT_BITSET with absolute CLOCK_MONOTONIC deadlines.
// A watchdog fails the run when no thread makes progress for 5 seconds
// (a lost wakeup), and every test checks its counters at the end.
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int seconds = 4;
static atomic_long progress;
static atomic_int stop;
static const char* current_test = "";

static long futex(atomic_int* addr, int op, int val, const struct timespec* ts, int val3) {
    return syscall(SYS_futex, addr, op, val, ts, NULL, val3);
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void* watchdog(void* arg) {
    (void)arg;
    long last = -1;
    double since = now();
    for (;;) {
        usleep(100 * 1000);
        long p = atomic_load(&progress);
        if (p != last) {
            last = p;
            since = now();
        } else if (now() - since > 5.0) {
            printf("  [FAIL] %s: no progress for 5 s (lost wakeup?), stop=%d\n", current_test, atomic_load(&stop));
            fflush(stdout);
            _exit(1);
        }
    }
    return NULL;
}

// ---- 1. a GIL like CPython's (Python/ceval_gil.c) ----
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond, switch_cond;
    int locked;
    atomic_int drop_request;
    unsigned long switches;
} gil = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0};

static void take_gil(void) {
    pthread_mutex_lock(&gil.mutex);
    while (gil.locked) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 5 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        unsigned long before = gil.switches;
        if (pthread_cond_timedwait(&gil.cond, &gil.mutex, &ts) == ETIMEDOUT && gil.locked &&
            gil.switches == before)
            atomic_store(&gil.drop_request, 1);
    }
    gil.locked = 1;
    gil.switches++;
    pthread_cond_signal(&gil.switch_cond);
    pthread_mutex_unlock(&gil.mutex);
}

static void drop_gil(void) {
    pthread_mutex_lock(&gil.mutex);
    gil.locked = 0;
    pthread_cond_signal(&gil.cond);
    pthread_mutex_unlock(&gil.mutex);
    if (atomic_exchange(&gil.drop_request, 0)) {
        // Like FORCE_SWITCHING: wait until someone else got it.
        pthread_mutex_lock(&gil.mutex);
        unsigned long s = gil.switches;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        while (gil.switches == s && !atomic_load(&stop))
            if (pthread_cond_timedwait(&gil.switch_cond, &gil.mutex, &ts) == ETIMEDOUT)
                break;
        pthread_mutex_unlock(&gil.mutex);
    }
}

static int pipefd[2];
static atomic_long gil_work[8];

static void* gil_thread(void* arg) {
    long id = (long)arg;
    char buf[256];
    while (!atomic_load(&stop)) {
        if (id == 0) {
            // The pipe reader: blocks in read() without the GIL, then
            // takes it to "process" the line.
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n <= 0)
                break;
        }
        take_gil();
        for (volatile int i = 0; i < 2000 + (int)id * 300; i++)
            ;
        atomic_fetch_add(&gil_work[id], 1);
        atomic_fetch_add(&progress, 1);
        drop_gil();
        if (id == 1)
            usleep(200); // the "main loop": short poll sleeps
    }
    return NULL;
}

static void* pipe_writer(void* arg) {
    (void)arg;
    int burst = 0;
    while (!atomic_load(&stop)) {
        for (int i = 0; i < (burst % 7) * 20 + 1; i++)
            if (write(pipefd[1], "dlstatus:1:12.5:Retrieving file\n", 32) < 0)
                return NULL;
        burst++;
        usleep(burst % 3 ? 100 : 3000);
    }
    close(pipefd[1]);
    return NULL;
}

static int test_gil(void) {
    current_test = "gil handoff";
    if (pipe(pipefd) < 0)
        return 1;
    pthread_t t[8], w;
    pthread_create(&w, NULL, pipe_writer, NULL);
    for (long i = 0; i < 8; i++)
        pthread_create(&t[i], NULL, gil_thread, (void*)i);
    sleep(seconds);
    atomic_store(&stop, 1);
    pthread_join(w, NULL);
    for (int i = 0; i < 8; i++)
        pthread_join(t[i], NULL);
    close(pipefd[0]);
    int bad = 0;
    for (int i = 0; i < 8; i++)
        if (atomic_load(&gil_work[i]) == 0)
            bad = 1;
    printf("  [%s] gil handoff: %lu switches, reader %ld, main %ld\n", bad ? "FAIL" : "PASS", gil.switches,
           atomic_load(&gil_work[0]), atomic_load(&gil_work[1]));
    atomic_store(&stop, 0);
    return bad;
}

// ---- 2. condition variable ping-pong ----
struct pair {
    pthread_mutex_t m;
    pthread_cond_t c;
    int turn;
    long rounds;
};

static void* pong(void* arg) {
    struct pair* p = arg;
    for (;;) {
        pthread_mutex_lock(&p->m);
        while (p->turn != 1 && !atomic_load(&stop))
            pthread_cond_wait(&p->c, &p->m);
        if (atomic_load(&stop)) {
            pthread_mutex_unlock(&p->m);
            return NULL;
        }
        p->turn = 0;
        pthread_cond_broadcast(&p->c);
        pthread_mutex_unlock(&p->m);
    }
}

static void* ping(void* arg) {
    struct pair* p = arg;
    while (!atomic_load(&stop)) {
        pthread_mutex_lock(&p->m);
        p->turn = 1;
        pthread_cond_broadcast(&p->c);
        while (p->turn != 0 && !atomic_load(&stop))
            pthread_cond_wait(&p->c, &p->m);
        p->rounds++;
        pthread_mutex_unlock(&p->m);
        atomic_fetch_add(&progress, 1);
    }
    pthread_mutex_lock(&p->m);
    pthread_cond_broadcast(&p->c);
    pthread_mutex_unlock(&p->m);
    return NULL;
}

static int test_pingpong(void) {
    current_test = "condvar ping-pong";
    enum { PAIRS = 6 };
    struct pair pairs[PAIRS];
    pthread_t a[PAIRS], b[PAIRS];
    for (int i = 0; i < PAIRS; i++) {
        pthread_mutex_init(&pairs[i].m, NULL);
        pthread_cond_init(&pairs[i].c, NULL);
        pairs[i].turn = 0;
        pairs[i].rounds = 0;
        pthread_create(&b[i], NULL, pong, &pairs[i]);
        pthread_create(&a[i], NULL, ping, &pairs[i]);
    }
    sleep(seconds);
    atomic_store(&stop, 1);
    long total = 0;
    int bad = 0;
    for (int i = 0; i < PAIRS; i++) { // wake both sides: they see `stop`
        pthread_mutex_lock(&pairs[i].m);
        pthread_cond_broadcast(&pairs[i].c);
        pthread_mutex_unlock(&pairs[i].m);
    }
    for (int i = 0; i < PAIRS; i++) {
        pthread_join(a[i], NULL);
        pthread_join(b[i], NULL);
        total += pairs[i].rounds;
        if (pairs[i].rounds == 0)
            bad = 1;
    }
    printf("  [%s] condvar ping-pong: %ld round trips\n", bad ? "FAIL" : "PASS", total);
    atomic_store(&stop, 0);
    return bad;
}

// ---- 3. raw futex: timeouts racing wakes ----
static atomic_int word;
static atomic_long woken, timed_out, waits;

static void* raw_waiter(void* arg) {
    long bitset_mode = (long)arg;
    while (!atomic_load(&stop)) {
        int v = atomic_load(&word);
        long r;
        if (bitset_mode) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            ts.tv_nsec += 20 * 1000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            r = futex(&word, FUTEX_WAIT_BITSET_PRIVATE, v, &ts, FUTEX_BITSET_MATCH_ANY);
        } else {
            struct timespec ts = {0, 20 * 1000};
            r = futex(&word, FUTEX_WAIT_PRIVATE, v, &ts, 0);
        }
        atomic_fetch_add(&waits, 1);
        if (r == 0)
            atomic_fetch_add(&woken, 1);
        else if (errno == ETIMEDOUT)
            atomic_fetch_add(&timed_out, 1);
        atomic_fetch_add(&progress, 1);
    }
    return NULL;
}

static void* raw_waker(void* arg) {
    (void)arg;
    while (!atomic_load(&stop)) {
        atomic_fetch_add(&word, 1);
        futex(&word, FUTEX_WAKE_PRIVATE, 1 + (atomic_load(&word) & 3), NULL, 0);
        if ((atomic_load(&word) & 63) == 0)
            usleep(10);
    }
    return NULL;
}

// A strict handoff on one futex word, no timeouts: a lost wake hangs it.
static atomic_int baton;
static atomic_long passes;

static void* baton_thread(void* arg) {
    int me = (int)(long)arg, n = 4;
    while (!atomic_load(&stop)) {
        int v;
        while ((v = atomic_load(&baton)) % n != me) {
            if (atomic_load(&stop))
                return NULL;
            struct timespec ts = {1, 0}; // only so the end of the test can't hang
            futex(&baton, FUTEX_WAIT_PRIVATE, v, &ts, 0);
        }
        atomic_fetch_add(&baton, 1);
        futex(&baton, FUTEX_WAKE_PRIVATE, n, NULL, 0);
        atomic_fetch_add(&passes, 1);
        atomic_fetch_add(&progress, 1);
    }
    return NULL;
}

static int test_raw(void) {
    current_test = "raw futex timeouts";
    pthread_t w[6], k[2];
    for (long i = 0; i < 6; i++)
        pthread_create(&w[i], NULL, raw_waiter, (void*)(i & 1));
    for (int i = 0; i < 2; i++)
        pthread_create(&k[i], NULL, raw_waker, NULL);
    sleep(seconds);
    atomic_store(&stop, 1);
    for (int i = 0; i < 2; i++)
        pthread_join(k[i], NULL);
    atomic_fetch_add(&word, 1);
    futex(&word, FUTEX_WAKE_PRIVATE, 100, NULL, 0);
    for (int i = 0; i < 6; i++)
        pthread_join(w[i], NULL);
    int bad = atomic_load(&waits) == 0; // (how many timed out depends on the machine)
    printf("  [%s] raw futex: %ld waits, %ld woken, %ld timed out\n", bad ? "FAIL" : "PASS", atomic_load(&waits),
           atomic_load(&woken), atomic_load(&timed_out));
    atomic_store(&stop, 0);

    current_test = "futex baton (4 threads, no timeouts)";
    pthread_t b[4];
    for (long i = 0; i < 4; i++)
        pthread_create(&b[i], NULL, baton_thread, (void*)i);
    sleep(seconds);
    atomic_store(&stop, 1);
    atomic_fetch_add(&baton, 1);
    futex(&baton, FUTEX_WAKE_PRIVATE, 100, NULL, 0);
    for (int i = 0; i < 4; i++)
        pthread_join(b[i], NULL);
    int bad2 = atomic_load(&passes) < 100;
    printf("  [%s] futex baton: %ld passes\n", bad2 ? "FAIL" : "PASS", atomic_load(&passes));
    atomic_store(&stop, 0);
    return bad + bad2;
}

// ---- 4. a signal for a thread that is just going to sleep in FUTEX_WAIT ----
static atomic_int never;  // nobody changes it: only the signal ends each wait
static atomic_long eintrs, handled;

// The handler runs when the thread leaves the kernel: a signal lost while
// it sleeps in FUTEX_WAIT never gets here.
static void on_usr1(int sig) { (void)sig; atomic_fetch_add(&handled, 1); }

static void* sig_sleeper(void* arg) {
    (void)arg;
    while (!atomic_load(&stop)) {
        if (futex(&never, FUTEX_WAIT_PRIVATE, 0, NULL, 0) < 0 && errno == EINTR)
            atomic_fetch_add(&eintrs, 1);
    }
    return NULL;
}

static int test_signal(void) {
    current_test = "signal vs FUTEX_WAIT";
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1; // no SA_RESTART: the wait must end with EINTR
    sigaction(SIGUSR1, &sa, NULL);
    pthread_t t;
    pthread_create(&t, NULL, sig_sleeper, NULL);
    double worst = 0, end = now() + seconds;
    long sent = 0;
    int bad = 0;
    while (now() < end) {
        long before = atomic_load(&handled);
        double t0 = now();
        pthread_kill(t, SIGUSR1);
        sent++;
        while (atomic_load(&handled) == before) {
            if (now() - t0 > 0.5) { bad = 1; break; } // the 1 s re-check would hide it otherwise
        }
        if (now() - t0 > worst)
            worst = now() - t0;
        atomic_fetch_add(&progress, 1);
        if (bad)
            break;
    }
    atomic_store(&stop, 1);
    pthread_kill(t, SIGUSR1);
    pthread_join(t, NULL);
    printf("  [%s] signal vs FUTEX_WAIT: %ld signals (%ld ended a wait), slowest %.1f ms\n", bad ? "FAIL" : "PASS",
           sent, atomic_load(&eintrs), worst * 1000);
    atomic_store(&stop, 0);
    return bad;
}

int main(int argc, char** argv) {
    if (argc > 1)
        seconds = atoi(argv[1]);
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("futexstress: %d s per test\n", seconds);
    pthread_t wd;
    pthread_create(&wd, NULL, watchdog, NULL);
    int fails = test_gil() + test_pingpong() + test_raw() + test_signal();
    printf("futexstress: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
