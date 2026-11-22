// Scheduler stress: timed sleeps woken by signals while threads and
// processes come and go, on every CPU.
//
//   schedstress [seconds]
//
// Sleepers block with a timeout (futex waits that time out, nanosleep);
// the main thread keeps signalling the process, which wakes every sleeping
// thread at once - also one that is still on its way to sleep, between
// marking itself asleep and arming its timeout. Meanwhile threads are
// created and joined and children forked and reaped, so the memory of
// finished threads is reused. A thread queued twice ran on two CPUs at once
// (the kernel faulted at RIP 0); a lost wakeup or hang trips the alarm.
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static atomic_int stop;
static atomic_long sleeps, churned, forks, signals;
static int never_changes;

static void on_signal(int sig) { (void)sig; }

static void *sleeper(void *arg) {
    long n = (long)arg;
    while (!atomic_load(&stop)) {
        struct timespec ts = {0, 200000 + (n % 4) * 100000}; // 0.2 - 0.5 ms
        if (n & 1)
            syscall(SYS_futex, &never_changes, FUTEX_WAIT_PRIVATE, 0, &ts, NULL, 0);
        else
            nanosleep(&ts, NULL);
        atomic_fetch_add(&sleeps, 1);
    }
    return NULL;
}

static void *short_lived(void *arg) {
    struct timespec ts = {0, 50000};
    nanosleep(&ts, NULL);
    return arg;
}

static void *churner(void *arg) {
    (void)arg;
    while (!atomic_load(&stop)) {
        pthread_t t[4];
        for (int i = 0; i < 4; i++)
            pthread_create(&t[i], NULL, short_lived, NULL);
        for (int i = 0; i < 4; i++)
            pthread_join(t[i], NULL);
        atomic_fetch_add(&churned, 4);
        pid_t c = fork();
        if (c == 0) {
            struct timespec ts = {0, 100000};
            nanosleep(&ts, NULL);
            _exit(0);
        }
        if (c > 0) {
            waitpid(c, NULL, 0);
            atomic_fetch_add(&forks, 1);
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
    alarm(seconds + 30); // a hang ends the test (SIGALRM kills it)
    pthread_t s[12], c[2];
    for (long i = 0; i < 12; i++)
        pthread_create(&s[i], NULL, sleeper, (void *)i);
    for (int i = 0; i < 2; i++)
        pthread_create(&c[i], NULL, churner, NULL);
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    do {
        kill(getpid(), SIGUSR1);
        atomic_fetch_add(&signals, 1);
        struct timespec ts = {0, 20000};
        nanosleep(&ts, NULL);
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (now.tv_sec - start.tv_sec < seconds);
    atomic_store(&stop, 1);
    for (int i = 0; i < 12; i++)
        pthread_join(s[i], NULL);
    for (int i = 0; i < 2; i++)
        pthread_join(c[i], NULL);
    printf("schedstress: ok (%d s: %ld timed sleeps, %ld signals, %ld threads, %ld forks)\n", seconds,
           atomic_load(&sleeps), atomic_load(&signals), atomic_load(&churned), atomic_load(&forks));
    return 0;
}
