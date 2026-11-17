// 32-bit (i386) POSIX threads: clone with a user_desc TLS descriptor,
// set_thread_area / %gs-based thread-local storage, futexes, signals to
// threads. Built with `gcc -m32 -static -pthread`.
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static int passed, failed;
static void check(int ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) passed++; else failed++;
}

#define THREADS 6
#define ITERATIONS 50000
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;
static __thread int tls_value; // lives behind %gs: every thread must see its own
static int tls_ok[THREADS];
static int tids[THREADS];

static void* worker(void* arg) {
    int id = (int)(long)arg;
    tls_value = id * 1000;
    tids[id] = (int)syscall(SYS_gettid);
    for (int i = 0; i < ITERATIONS; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
        if (tls_value != id * 1000) // another thread's TLS leaking in = broken %gs switching
            return NULL;
        if ((i & 1023) == 0)
            sched_yield();
    }
    tls_ok[id] = 1;
    return (void*)(long)(id + 100);
}

static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int ready;
static void* waiter(void* arg) {
    (void)arg;
    pthread_mutex_lock(&lock);
    while (!ready)
        pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);
    return (void*)7;
}

static volatile sig_atomic_t usr1_tid;
static void on_usr1(int sig) { (void)sig; usr1_tid = (int)syscall(SYS_gettid); }
static sem_t sem;
static void* sig_target(void* arg) {
    (void)arg;
    sem_post(&sem);
    while (!usr1_tid)
        usleep(1000);
    return NULL;
}

int main(void) {
    printf("ia32threads: %d threads x %d locked increments (32-bit)\n", THREADS, ITERATIONS);
    tls_value = -1;
    pthread_t t[THREADS];
    int created = 0;
    for (long i = 0; i < THREADS; i++)
        created += pthread_create(&t[i], NULL, worker, (void*)i) == 0;
    check(created == THREADS, "pthread_create x6 (clone + user_desc TLS)");
    int joined = 0;
    for (int i = 0; i < THREADS; i++) {
        void* ret;
        joined += pthread_join(t[i], &ret) == 0 && ret == (void*)(long)(i + 100);
    }
    check(joined == THREADS, "pthread_join returns each thread's value");
    check(counter == (long)THREADS * ITERATIONS, "mutex-protected counter is exact");
    int all_tls = 1, distinct = 1;
    for (int i = 0; i < THREADS; i++) {
        all_tls &= tls_ok[i];
        for (int j = 0; j < i; j++)
            distinct &= tids[i] != tids[j];
    }
    check(all_tls && tls_value == -1, "__thread variables are per thread (%gs TLS)");
    check(distinct, "every thread has its own tid");

    pthread_t w;
    pthread_create(&w, NULL, waiter, NULL);
    usleep(20000);
    pthread_mutex_lock(&lock);
    ready = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    void* wret;
    check(pthread_join(w, &wret) == 0 && wret == (void*)7, "condition variable wakeup (futex)");

    signal(SIGUSR1, on_usr1);
    sem_init(&sem, 0, 0);
    pthread_t st;
    pthread_create(&st, NULL, sig_target, NULL);
    sem_wait(&sem);
    pthread_kill(st, SIGUSR1);
    pthread_join(st, NULL);
    check(usr1_tid != 0 && usr1_tid != (int)syscall(SYS_gettid), "pthread_kill runs the handler on the target thread");

    printf("ia32threads: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
