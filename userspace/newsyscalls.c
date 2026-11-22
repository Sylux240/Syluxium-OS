// Checks for copy_file_range, pidfd_open / pidfd_send_signal,
// rt_sigtimedwait and mlock/munlock. Prints PASS/FAIL per check; exit
// status = number of failures.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
static void check(int ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        fails++;
}

static void test_copy_file_range(void) {
    const char* a = "/tmp/cfr-in", *b = "/tmp/cfr-out";
    int in = open(a, O_RDWR | O_CREAT | O_TRUNC, 0644);
    char data[200000];
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = (char)(i * 7 + i / 300);
    check(in >= 0 && write(in, data, sizeof(data)) == (ssize_t)sizeof(data), "copy_file_range: source written");
    int out = open(b, O_RDWR | O_CREAT | O_TRUNC, 0644);
    loff_t off_in = 0;
    ssize_t n = syscall(SYS_copy_file_range, in, &off_in, out, NULL, sizeof(data), 0);
    check(n == (ssize_t)sizeof(data) && off_in == (loff_t)sizeof(data), "copy_file_range: whole file copied");
    char back[sizeof(data)];
    check(pread(out, back, sizeof(back), 0) == (ssize_t)sizeof(back) && memcmp(back, data, sizeof(data)) == 0,
          "copy_file_range: same bytes");
    check(lseek(out, 0, SEEK_CUR) == (off_t)sizeof(data), "copy_file_range: output position advanced");
    loff_t o1 = 100, o2 = 150;
    check(syscall(SYS_copy_file_range, in, &o1, in, &o2, 100, 0) < 0 && errno == EINVAL,
          "copy_file_range: overlapping ranges refused");
    close(in);
    close(out);
    unlink(a);
    unlink(b);
}

static void test_pidfd(void) {
    pid_t child = fork();
    if (child == 0) {
        usleep(200 * 1000);
        _exit(3);
    }
    int pfd = syscall(SYS_pidfd_open, child, 0);
    check(pfd >= 0, "pidfd_open on a child");
    struct pollfd p = {pfd, POLLIN, 0};
    check(poll(&p, 1, 0) == 0, "pidfd: not readable while the child runs");
    check(poll(&p, 1, 3000) == 1 && (p.revents & POLLIN), "pidfd: readable when the child exits");
    int st;
    check(waitpid(child, &st, 0) == child && WIFEXITED(st) && WEXITSTATUS(st) == 3, "pidfd: child reaped");
    close(pfd);

    child = fork();
    if (child == 0) {
        for (;;)
            pause();
    }
    pfd = syscall(SYS_pidfd_open, child, 0);
    check(syscall(SYS_pidfd_send_signal, pfd, SIGTERM, NULL, 0) == 0, "pidfd_send_signal SIGTERM");
    check(waitpid(child, &st, 0) == child && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM,
          "pidfd_send_signal: the child died of it");
    check(syscall(SYS_pidfd_send_signal, pfd, SIGTERM, NULL, 0) < 0 && errno == ESRCH,
          "pidfd_send_signal to a reaped process: ESRCH");
    close(pfd);
    check(syscall(SYS_pidfd_open, 999999, 0) < 0 && errno == ESRCH, "pidfd_open of no process: ESRCH");

    /* waitid(P_PIDFD): how GLib (Xfce's session) watches its children. */
    child = fork();
    if (child == 0) {
        usleep(100 * 1000);
        _exit(7);
    }
    pfd = syscall(SYS_pidfd_open, child, 0);
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    check(waitid(3 /* P_PIDFD */, pfd, &si, WEXITED) == 0 && si.si_pid == child && si.si_code == CLD_EXITED &&
              si.si_status == 7, "waitid(P_PIDFD) reaps the child");
    close(pfd);
}

static void test_sigtimedwait(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);
    kill(getpid(), SIGUSR1);
    siginfo_t info;
    struct timespec ts = {1, 0};
    int sig = sigtimedwait(&set, &info, &ts);
    check(sig == SIGUSR1 && info.si_signo == SIGUSR1 && info.si_pid == getpid(), "sigtimedwait: pending signal taken");
    struct timespec short_ts = {0, 50 * 1000 * 1000};
    check(sigtimedwait(&set, &info, &short_ts) < 0 && errno == EAGAIN, "sigtimedwait: times out (EAGAIN)");
    pid_t child = fork();
    if (child == 0) {
        usleep(100 * 1000);
        kill(getppid(), SIGUSR1);
        _exit(0);
    }
    check(sigwaitinfo(&set, &info) == SIGUSR1 && info.si_pid == child, "sigwaitinfo: waits for a signal from a child");
    waitpid(child, NULL, 0);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
}

static void test_mlock(void) {
    char* p = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(mlock(p, 65536) == 0 && munlock(p, 65536) == 0, "mlock / munlock");
    munmap(p, 65536);
}

// ASLR: two runs of a program get different stack, heap, code and mmap
// addresses; with personality(ADDR_NO_RANDOMIZE) (setarch -R) they don't.
static void run_addrs(int norandom, char* out, size_t n) {
    int p[2];
    if (pipe(p) < 0)
        return;
    pid_t c = fork();
    if (c == 0) {
        if (norandom)
            personality(ADDR_NO_RANDOMIZE);
        dup2(p[1], 1);
        execl("/proc/self/exe", "newsyscalls", "addrs", (char*)NULL);
        _exit(127);
    }
    close(p[1]);
    ssize_t r = read(p[0], out, n - 1);
    out[r > 0 ? r : 0] = 0;
    close(p[0]);
    waitpid(c, NULL, 0);
}

static void test_aslr(void) {
    char a[200], b[200], c[200], d[200];
    run_addrs(0, a, sizeof(a));
    run_addrs(0, b, sizeof(b));
    run_addrs(1, c, sizeof(c));
    run_addrs(1, d, sizeof(d));
    check(a[0] && strcmp(a, b) != 0, "ASLR: two runs get different addresses");
    check(c[0] && strcmp(c, d) == 0, "ASLR off with personality(ADDR_NO_RANDOMIZE)");
}

// POSIX timers: a signal carrying the timer's value, a thread callback
// (SIGEV_THREAD, done by the C library), a periodic timer.
static volatile sig_atomic_t timer_hits, timer_value_ok;
static void on_timer(int sig, siginfo_t* si, void* uc) {
    (void)sig; (void)uc;
    timer_hits++;
    if (si->si_code == SI_TIMER && si->si_value.sival_int == 4242)
        timer_value_ok = 1;
}
static volatile int thread_calls;
static void timer_thread_fn(union sigval v) {
    if (v.sival_int == 77)
        __atomic_add_fetch(&thread_calls, 1, __ATOMIC_SEQ_CST);
}

static void test_posix_timers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_timer;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGRTMIN + 1, &sa, NULL);
    struct sigevent ev;
    memset(&ev, 0, sizeof(ev));
    ev.sigev_notify = SIGEV_SIGNAL;
    ev.sigev_signo = SIGRTMIN + 1;
    ev.sigev_value.sival_int = 4242;
    timer_t t;
    check(timer_create(CLOCK_MONOTONIC, &ev, &t) == 0, "timer_create (signal)");
    struct itimerspec its = {{0, 20 * 1000 * 1000}, {0, 20 * 1000 * 1000}}; // every 20 ms
    check(timer_settime(t, 0, &its, NULL) == 0, "timer_settime periodic");
    for (int i = 0; i < 100 && timer_hits < 5; i++)
        usleep(20 * 1000);
    struct itimerspec cur;
    check(timer_gettime(t, &cur) == 0 && cur.it_interval.tv_nsec == 20 * 1000 * 1000, "timer_gettime");
    check(timer_hits >= 5 && timer_value_ok, "timer signals arrive with si_value");
    check(timer_delete(t) == 0, "timer_delete");

    memset(&ev, 0, sizeof(ev));
    ev.sigev_notify = SIGEV_THREAD;
    ev.sigev_notify_function = timer_thread_fn;
    ev.sigev_value.sival_int = 77;
    check(timer_create(CLOCK_REALTIME, &ev, &t) == 0, "timer_create (SIGEV_THREAD)");
    struct itimerspec once = {{0, 0}, {0, 30 * 1000 * 1000}};
    timer_settime(t, 0, &once, NULL);
    for (int i = 0; i < 100 && !thread_calls; i++)
        usleep(10 * 1000);
    check(thread_calls == 1, "SIGEV_THREAD timer runs its function");
    timer_delete(t);
}

// process_vm_readv / writev on a child.
static void test_process_vm(void) {
    static char secret[64] = "nothing yet";
    int p[2];
    if (pipe(p) < 0)
        return;
    pid_t c = fork();
    if (c == 0) {
        strcpy(secret, "child's secret");
        (void)!write(p[1], "x", 1);
        pause();
        _exit(0);
    }
    char x;
    (void)!read(p[0], &x, 1);
    char buf[64] = {0};
    struct iovec l = {buf, sizeof(buf)}, r = {secret, sizeof(secret)};
    check(process_vm_readv(c, &l, 1, &r, 1, 0) == (ssize_t)sizeof(secret) && strcmp(buf, "child's secret") == 0,
          "process_vm_readv reads another process's memory");
    char news[64] = "written by the parent";
    struct iovec l2 = {news, sizeof(news)};
    check(process_vm_writev(c, &l2, 1, &r, 1, 0) == (ssize_t)sizeof(news), "process_vm_writev");
    memset(buf, 0, sizeof(buf));
    check(process_vm_readv(c, &l, 1, &r, 1, 0) > 0 && strcmp(buf, "written by the parent") == 0,
          "process_vm_writev's data is there");
    kill(c, SIGKILL);
    waitpid(c, NULL, 0);
    close(p[0]);
    close(p[1]);
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "addrs") == 0) {
        int local;
        void* heap = malloc(16);
        void* map = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        printf("%p %p %p %p\n", (void*)&local, heap, (void*)main, map);
        return 0;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    test_copy_file_range();
    test_pidfd();
    test_sigtimedwait();
    test_mlock();
    test_aslr();
    test_posix_timers();
    test_process_vm();
    printf("newsyscalls: %d failed\n", fails);
    return fails;
}
