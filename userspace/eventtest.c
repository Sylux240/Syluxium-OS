// epoll, eventfd, timerfd, signalfd: the event APIs GLib, Wayland and D-Bus use.
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(c, what) do { if (c) printf("  [ ok ] %s\n", what); else { printf("  [FAIL] %s (errno %d %s)\n", what, errno, strerror(errno)); failures++; } } while (0)

static long ms_since(struct timespec* t0) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

int main(void) {
    printf("eventtest: epoll, eventfd, timerfd, signalfd\n");

    // eventfd basics + semaphore mode
    int efd = eventfd(3, EFD_NONBLOCK);
    uint64_t v = 0;
    CHECK(efd >= 0 && read(efd, &v, 8) == 8 && v == 3, "eventfd: initial value read");
    CHECK(read(efd, &v, 8) == -1 && errno == EAGAIN, "eventfd: empty -> EAGAIN");
    int sem = eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
    CHECK(read(sem, &v, 8) == 8 && v == 1 && read(sem, &v, 8) == 8 && v == 1 && read(sem, &v, 8) == -1,
          "eventfd: semaphore mode counts down by one");

    // epoll: level-triggered on a pipe and an eventfd
    int ep = epoll_create1(EPOLL_CLOEXEC);
    int p[2];
    pipe(p);
    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 111}, out[8];
    CHECK(ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0, "epoll_create1 + EPOLL_CTL_ADD");
    ev.data.u64 = 222;
    epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) == -1 && errno == EEXIST, "adding twice -> EEXIST");
    CHECK(epoll_wait(ep, out, 8, 0) == 0, "nothing ready -> 0");
    write(p[1], "x", 1);
    v = 5;
    write(efd, &v, 8);
    int n = epoll_wait(ep, out, 8, 100);
    int seen_pipe = 0, seen_efd = 0;
    for (int i = 0; i < n; i++) {
        if (out[i].data.u64 == 111) seen_pipe = 1;
        if (out[i].data.u64 == 222) seen_efd = 1;
    }
    CHECK(n == 2 && seen_pipe && seen_efd, "both sources reported with their data");
    CHECK(epoll_wait(ep, out, 8, 0) == 2, "level-triggered: still ready until consumed");
    char c;
    read(p[0], &c, 1);
    read(efd, &v, 8);
    CHECK(epoll_wait(ep, out, 8, 0) == 0, "consumed -> not ready");

    // blocking wait woken by another process
    if (fork() == 0) { usleep(50000); write(p[1], "y", 1); _exit(0); }
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    n = epoll_wait(ep, out, 8, 2000);
    CHECK(n == 1 && out[0].data.u64 == 111 && ms_since(&t0) >= 40, "blocking epoll_wait woken by a write from a child");
    read(p[0], &c, 1);
    wait(NULL);

    // edge-triggered + oneshot
    int ep2 = epoll_create1(0);
    int q[2];
    pipe(q);
    ev.events = EPOLLIN | EPOLLET;
    ev.data.u64 = 333;
    epoll_ctl(ep2, EPOLL_CTL_ADD, q[0], &ev);
    write(q[1], "ab", 2);
    CHECK(epoll_wait(ep2, out, 8, 0) == 1 && epoll_wait(ep2, out, 8, 0) == 0, "EPOLLET: reported once per edge");
    write(q[1], "c", 1);
    CHECK(epoll_wait(ep2, out, 8, 0) == 1, "EPOLLET: new data -> new edge");
    ev.events = EPOLLIN | EPOLLONESHOT;
    epoll_ctl(ep2, EPOLL_CTL_MOD, q[0], &ev);
    CHECK(epoll_wait(ep2, out, 8, 0) == 1 && epoll_wait(ep2, out, 8, 0) == 0, "EPOLLONESHOT: disabled after one report");
    CHECK(epoll_ctl(ep2, EPOLL_CTL_MOD, q[0], &ev) == 0 && epoll_wait(ep2, out, 8, 0) == 1, "EPOLL_CTL_MOD re-arms it");

    // nested epoll: ep2 inside ep (re-arm the one-shot item so ep2 has something ready)
    epoll_ctl(ep2, EPOLL_CTL_MOD, q[0], &ev);
    ev.events = EPOLLIN;
    ev.data.u64 = 444;
    epoll_ctl(ep, EPOLL_CTL_ADD, ep2, &ev);
    n = epoll_wait(ep, out, 8, 0);
    CHECK(n == 1 && out[0].data.u64 == 444, "nested epoll: inner set readiness propagates");
    epoll_ctl(ep, EPOLL_CTL_DEL, ep2, NULL);
    close(q[0]);
    CHECK(epoll_wait(ep2, out, 8, 0) == 0, "closing a watched fd removes it from the set");

    // timerfd: one-shot and periodic
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec its = {.it_value = {0, 30 * 1000000}, .it_interval = {0, 0}};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    timerfd_settime(tfd, 0, &its, NULL);
    CHECK(read(tfd, &v, 8) == 8 && v == 1 && ms_since(&t0) >= 28, "timerfd one-shot fires after 30 ms (blocking read)");
    its.it_value.tv_nsec = 10 * 1000000;
    its.it_interval.tv_nsec = 10 * 1000000;
    timerfd_settime(tfd, 0, &its, NULL);
    ev.events = EPOLLIN;
    ev.data.u64 = 555;
    epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);
    n = epoll_wait(ep, out, 8, 1000);
    CHECK(n == 1 && out[0].data.u64 == 555, "periodic timerfd wakes epoll_wait");
    usleep(55000);
    read(tfd, &v, 8);
    CHECK(v >= 4, "periodic timerfd counts missed expirations");
    struct itimerspec cur;
    timerfd_gettime(tfd, &cur);
    CHECK(cur.it_interval.tv_nsec == 10 * 1000000, "timerfd_gettime reports the interval");
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_nsec = 0;
    timerfd_settime(tfd, 0, &its, NULL);
    epoll_ctl(ep, EPOLL_CTL_DEL, tfd, NULL);

    // signalfd for SIGCHLD (blocked; default action is "ignore")
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
    ev.data.u64 = 666;
    epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &ev);
    pid_t child = fork();
    if (child == 0) _exit(7);
    n = epoll_wait(ep, out, 8, 2000);
    struct signalfd_siginfo si;
    CHECK(n == 1 && out[0].data.u64 == 666 && read(sfd, &si, sizeof(si)) == sizeof(si) && si.ssi_signo == SIGCHLD,
          "signalfd delivers a blocked SIGCHLD through epoll");
    int status;
    CHECK(waitpid(child, &status, 0) == child && WEXITSTATUS(status) == 7, "child reaped after signalfd");

    // epoll_pwait with a temporary mask that unblocks SIGUSR1
    signal(SIGUSR1, SIG_DFL);
    sigset_t block_usr1, empty;
    sigemptyset(&block_usr1);
    sigaddset(&block_usr1, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block_usr1, NULL);
    sigemptyset(&empty);
    sigaddset(&empty, SIGCHLD);
    struct sigaction sa = {0};
    static volatile int got_usr1 = 0;
    void handler(int s) { (void)s; got_usr1 = 1; }
    sa.sa_handler = handler;
    sigaction(SIGUSR1, &sa, NULL);
    if (fork() == 0) { usleep(30000); kill(getppid(), SIGUSR1); _exit(0); }
    n = epoll_pwait(ep, out, 8, 2000, &empty);
    sigset_t now;
    sigprocmask(SIG_BLOCK, NULL, &now);
    CHECK(n == -1 && errno == EINTR && got_usr1 && sigismember(&now, SIGUSR1),
          "epoll_pwait: temporary mask lets SIGUSR1 in, old mask restored after the handler");
    while (waitpid(-1, NULL, WNOHANG) > 0) ;

    printf("eventtest: %s (%d failure%s)\n", failures ? "FAILED" : "all passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
