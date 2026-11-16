// POSIX threads through glibc: exercises clone(CLONE_THREAD), futexes,
// thread-local storage, per-thread signals and exit_group.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int passed, failed;
static void check(int ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) passed++; else failed++;
}

// ---- mutex: many threads, one counter ----
#define THREADS 8
#define ITERATIONS 100000
static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;
static int cpu_seen[256];

static void* increment(void* arg) {
    (void)arg;
    for (int i = 0; i < ITERATIONS; i++) {
        pthread_mutex_lock(&counter_lock);
        counter++;
        pthread_mutex_unlock(&counter_lock);
        if ((i & 4095) == 0) {
            int cpu = sched_getcpu();
            if (cpu >= 0 && cpu < 256) __atomic_store_n(&cpu_seen[cpu], 1, __ATOMIC_RELAXED);
        }
    }
    return NULL;
}

// ---- condition variable: producer / consumers ----
#define ITEMS 20000
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_nonempty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t q_nonfull = PTHREAD_COND_INITIALIZER;
static int queue[64], q_head, q_count, produced_all;
static long consumed_sum;

static void* producer(void* arg) {
    (void)arg;
    for (int i = 1; i <= ITEMS; i++) {
        pthread_mutex_lock(&q_lock);
        while (q_count == 64) pthread_cond_wait(&q_nonfull, &q_lock);
        queue[(q_head + q_count++) % 64] = i;
        pthread_cond_signal(&q_nonempty);
        pthread_mutex_unlock(&q_lock);
    }
    pthread_mutex_lock(&q_lock);
    produced_all = 1;
    pthread_cond_broadcast(&q_nonempty);
    pthread_mutex_unlock(&q_lock);
    return NULL;
}

static void* consumer(void* arg) {
    (void)arg;
    long local = 0;
    for (;;) {
        pthread_mutex_lock(&q_lock);
        while (q_count == 0 && !produced_all) pthread_cond_wait(&q_nonempty, &q_lock);
        if (q_count == 0 && produced_all) { pthread_mutex_unlock(&q_lock); break; }
        local += queue[q_head];
        q_head = (q_head + 1) % 64;
        q_count--;
        pthread_cond_signal(&q_nonfull);
        pthread_mutex_unlock(&q_lock);
    }
    __atomic_add_fetch(&consumed_sum, local, __ATOMIC_RELAXED);
    return NULL;
}

// ---- thread-local storage ----
static __thread int tls_value = 7;
static void* tls_worker(void* arg) {
    tls_value = (int)(long)arg;
    usleep(10000);
    return (void*)(long)(tls_value == (int)(long)arg);
}

// ---- per-thread signal ----
static pthread_t signal_target;
static volatile int handler_ran_in_target;
static void on_usr1(int sig) {
    (void)sig;
    handler_ran_in_target = pthread_equal(pthread_self(), signal_target);
}
static void* wait_for_signal(void* arg) {
    (void)arg;
    for (int i = 0; i < 200 && !handler_ran_in_target; i++) usleep(5000);
    return NULL;
}

static pid_t main_tid;
static void* report_tid(void* arg) {
    *(pid_t*)arg = (pid_t)syscall(SYS_gettid);
    return NULL;
}

static void* exit_whole_process(void* arg) {
    (void)arg;
    usleep(20000);
    exit(5); // ends every thread, not just this one
}

int main(void) {
    printf("threadtest: pid %d\n", getpid());

    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++) pthread_create(&t[i], NULL, increment, NULL);
    for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
    int cpus = 0;
    for (int i = 0; i < 256; i++) cpus += cpu_seen[i];
    char desc[96];
    snprintf(desc, sizeof(desc), "mutex: %d threads x %d increments (ran on %d CPUs)", THREADS, ITERATIONS, cpus);
    check(counter == (long)THREADS * ITERATIONS, desc);

    pthread_t prod, cons[3];
    pthread_create(&prod, NULL, producer, NULL);
    for (int i = 0; i < 3; i++) pthread_create(&cons[i], NULL, consumer, NULL);
    pthread_join(prod, NULL);
    for (int i = 0; i < 3; i++) pthread_join(cons[i], NULL);
    check(consumed_sum == (long)ITEMS * (ITEMS + 1) / 2, "condition variables: producer + 3 consumers");

    int tls_ok = 1;
    pthread_t tt[4];
    for (int i = 0; i < 4; i++) pthread_create(&tt[i], NULL, tls_worker, (void*)(long)(100 + i));
    for (int i = 0; i < 4; i++) { void* r; pthread_join(tt[i], &r); tls_ok &= r != NULL; }
    check(tls_ok && tls_value == 7, "thread-local storage (__thread)");

    main_tid = (pid_t)syscall(SYS_gettid);
    pid_t other_tid = 0;
    pthread_t tidt;
    pthread_create(&tidt, NULL, report_tid, &other_tid);
    pthread_join(tidt, NULL);
    check(main_tid == getpid() && other_tid != 0 && other_tid != main_tid, "gettid: distinct per thread");

    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t c = PTHREAD_COND_INITIALIZER;
    struct timespec deadline, start, end;
    clock_gettime(CLOCK_REALTIME, &deadline);
    clock_gettime(CLOCK_MONOTONIC, &start);
    deadline.tv_nsec += 50 * 1000 * 1000;
    if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&m);
    int rc = pthread_cond_timedwait(&c, &m, &deadline);
    pthread_mutex_unlock(&m);
    clock_gettime(CLOCK_MONOTONIC, &end);
    double ms = (end.tv_sec - start.tv_sec) * 1e3 + (end.tv_nsec - start.tv_nsec) / 1e6;
    check(rc == ETIMEDOUT && ms >= 45 && ms < 1000, "pthread_cond_timedwait times out (futex + timeout)");

    sem_t sem;
    sem_init(&sem, 0, 0);
    sem_post(&sem);
    check(sem_trywait(&sem) == 0 && sem_trywait(&sem) == -1 && errno == EAGAIN, "semaphores");

    signal(SIGUSR1, on_usr1);
    pthread_create(&signal_target, NULL, wait_for_signal, NULL);
    pthread_kill(signal_target, SIGUSR1);
    pthread_join(signal_target, NULL);
    check(handler_ran_in_target, "pthread_kill: handler runs in the target thread");

    pid_t child = fork();
    if (child == 0) {
        pthread_t ex;
        pthread_create(&ex, NULL, exit_whole_process, NULL);
        for (;;) pause();
    }
    int status = 0;
    waitpid(child, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 5, "exit() from a thread ends the whole process");

    printf("threadtest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
