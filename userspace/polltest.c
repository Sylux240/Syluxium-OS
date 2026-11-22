// poll() wakeups under ping-pong load, the way Firefox's processes and
// threads talk (Unix socket pairs, pipes, eventfds; poll with no timeout).
// A lost wakeup shows up as a stall: each round trip should take
// microseconds, and the test reports the slowest one.
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define ROUNDS 20000

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void wait_readable(int fd) {
    struct pollfd p = {fd, POLLIN, 0};
    while (poll(&p, 1, -1) < 0 && errno == EINTR)
        ;
}

// One side: wait (poll, no timeout) then read one byte, write one byte back.
static void echo(int rfd, int wfd, int rounds) {
    char c;
    for (int i = 0; i < rounds; i++) {
        wait_readable(rfd);
        if (read(rfd, &c, 1) != 1)
            _exit(2);
        if (write(wfd, &c, 1) != 1)
            _exit(3);
    }
}

static void report(const char* what, double total, double worst) {
    int ok = worst < 500.0; // a lost wakeup costs a timeout somewhere: seconds
    printf("  [%s] %s: %d round trips in %.0f ms, slowest %.1f ms\n", ok ? "PASS" : "FAIL", what, ROUNDS, total, worst);
    if (!ok)
        failures++;
}

static void ping(const char* what, int rfd, int wfd) {
    double start = now_ms(), worst = 0;
    char c = 'x';
    for (int i = 0; i < ROUNDS; i++) {
        double t0 = now_ms();
        if (write(wfd, &c, 1) != 1) { printf("  [FAIL] %s: write\n", what); failures++; return; }
        wait_readable(rfd);
        if (read(rfd, &c, 1) != 1) { printf("  [FAIL] %s: read\n", what); failures++; return; }
        double d = now_ms() - t0;
        if (d > worst)
            worst = d;
    }
    report(what, now_ms() - start, worst);
}

static void processes_socketpair(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    pid_t c = fork();
    if (!c) { close(sv[0]); echo(sv[1], sv[1], ROUNDS); _exit(0); }
    close(sv[1]);
    ping("processes, Unix stream socket pair", sv[0], sv[0]);
    waitpid(c, NULL, 0);
    close(sv[0]);
}

static void processes_seqpacket(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);
    pid_t c = fork();
    if (!c) { close(sv[0]); echo(sv[1], sv[1], ROUNDS); _exit(0); }
    close(sv[1]);
    ping("processes, Unix seqpacket socket pair", sv[0], sv[0]);
    waitpid(c, NULL, 0);
    close(sv[0]);
}

static void processes_pipes(void) {
    int a[2], b[2];
    pipe(a);
    pipe(b);
    pid_t c = fork();
    if (!c) { echo(a[0], b[1], ROUNDS); _exit(0); }
    ping("processes, two pipes", b[0], a[1]);
    waitpid(c, NULL, 0);
}

struct targs { int rfd, wfd; };
static void* thread_echo(void* p) {
    struct targs* t = p;
    echo(t->rfd, t->wfd, ROUNDS);
    return NULL;
}

static void threads_socketpair(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    struct targs t = {sv[1], sv[1]};
    pthread_t th;
    pthread_create(&th, NULL, thread_echo, &t);
    ping("threads, Unix stream socket pair", sv[0], sv[0]);
    pthread_join(th, NULL);
}

// eventfd: the other thread waits on it and answers through a pipe.
static int efd, epipe[2];
static void* thread_eventfd(void* p) {
    (void)p;
    uint64_t v;
    for (int i = 0; i < ROUNDS; i++) {
        wait_readable(efd);
        if (read(efd, &v, 8) != 8)
            return NULL;
        char c = 'e';
        if (write(epipe[1], &c, 1) != 1)
            return NULL;
    }
    return NULL;
}

static void threads_eventfd(void) {
    efd = eventfd(0, 0);
    pipe(epipe);
    pthread_t th;
    pthread_create(&th, NULL, thread_eventfd, NULL);
    double start = now_ms(), worst = 0;
    for (int i = 0; i < ROUNDS; i++) {
        double t0 = now_ms();
        uint64_t one = 1;
        char c;
        if (write(efd, &one, 8) != 8) break;
        wait_readable(epipe[0]);
        if (read(epipe[0], &c, 1) != 1) break;
        double d = now_ms() - t0;
        if (d > worst)
            worst = d;
    }
    report("threads, eventfd + pipe", now_ms() - start, worst);
    pthread_join(th, NULL);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    processes_socketpair();
    processes_seqpacket();
    processes_pipes();
    threads_socketpair();
    threads_eventfd();
    printf("polltest: %s\n", failures ? "FAILED" : "all passed");
    return failures != 0;
}
