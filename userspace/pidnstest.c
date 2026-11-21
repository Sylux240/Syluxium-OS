// PID namespaces (clone(CLONE_NEWPID)), as Android's init in a container
// needs them:
//   - the first process is pid 1 (getppid 0), its children 2, 3, ...
//   - /proc/self, /proc listings and kill() use the namespace's numbers;
//     processes outside are invisible
//   - orphans are adopted and reaped by the namespace's init
//   - when init exits, everything else in the namespace is killed
// Needs root.
#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("pidnstest: FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static pid_t clone_newpid(void) { return (pid_t)syscall(SYS_clone, CLONE_NEWPID | SIGCHLD, NULL, NULL, NULL, 0); }

static volatile pid_t sig_from = -1;
static void on_usr1(int sig, siginfo_t* si, void* ctx) { sig_from = si->si_pid; }

// pid 1 of the namespace.
static int ns_init(pid_t outside_parent) {
    char buf[32];
    CHECK(getpid() == 1, "getpid in the namespace: %d", getpid());
    CHECK(getppid() == 0, "getppid of the namespace's init: %d", getppid());
    ssize_t n = readlink("/proc/self", buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = 0;
    CHECK(!strcmp(buf, "1"), "/proc/self is %s", buf);
    CHECK(kill(outside_parent, 0) < 0 && errno == ESRCH, "the parent outside is visible as %d", outside_parent);

    // A child: pid 2. It leaves an orphan behind and exits.
    int p[2];
    if (pipe(p) < 0)
        return 1;
    pid_t c = fork();
    if (c == 0) {
        close(p[0]);
        if (getpid() != 2 || getppid() != 1)
            _exit(10);
        pid_t orphan = fork();
        if (orphan == 0) {
            // Wait until our parent is gone: init adopts us.
            for (int i = 0; i < 200 && getppid() != 1; i++)
                usleep(10000);
            char ok = getppid() == 1 && getpid() == 3;
            if (write(p[1], &ok, 1) != 1) { }
            _exit(0);
        }
        _exit(0);
    }
    close(p[1]);
    CHECK(c == 2, "first child's pid as init sees it: %d", c);
    char ok = 0;
    CHECK(read(p[0], &ok, 1) == 1 && ok, "orphan adopted by init (pid 3, parent 1)");
    close(p[0]);
    // Reap both: the child and the adopted orphan.
    int reaped = 0, st;
    pid_t w;
    while ((w = wait(&st)) > 0)
        reaped++;
    CHECK(reaped == 2, "init reaped %d processes (2 expected)", reaped);

    // /proc lists only this namespace's processes.
    DIR* d = opendir("/proc");
    int others = 0;
    for (struct dirent* e; d && (e = readdir(d));)
        if (e->d_name[0] >= '1' && e->d_name[0] <= '9' && atoi(e->d_name) != 1)
            others++;
    if (d)
        closedir(d);
    CHECK(others == 0, "/proc shows %d other processes", others);

    // A signal from a process inside: si_pid in our numbers.
    struct sigaction sa = {.sa_sigaction = on_usr1, .sa_flags = SA_SIGINFO};
    sigaction(SIGUSR1, &sa, NULL);
    pid_t sender = fork();
    if (sender == 0) {
        kill(1, SIGUSR1);
        _exit(0);
    }
    waitpid(sender, &st, 0);
    for (int i = 0; i < 100 && sig_from < 0; i++)
        usleep(1000);
    CHECK(sig_from == sender, "si_pid %d, sender %d", sig_from, sender);

    // A sleeper that will be killed when we exit; the parent outside watches
    // the pipe it holds (fd 3).
    if (fork() == 0) {
        for (;;)
            pause();
    }
    return failures;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (geteuid() != 0) {
        printf("pidnstest: needs root\n");
        return 1;
    }
    int watch[2];
    if (pipe(watch) < 0)
        return 1;
    pid_t me = getpid();
    pid_t init = clone_newpid();
    if (init == 0) {
        close(watch[0]);
        // Keep the write end open (the sleeper inherits it).
        _exit(ns_init(me));
    }
    close(watch[1]);
    CHECK(init > 1, "clone(CLONE_NEWPID) returned %d (%s)", init, strerror(errno));
    int st = 0;
    CHECK(waitpid(init, &st, 0) == init, "waitpid on the namespace's init");
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "init's checks (exit status %#x)", st);
    // The sleeper died with init: the pipe's last writer is gone.
    char c;
    alarm(5);
    CHECK(read(watch[0], &c, 1) == 0, "the sleeper outlived the namespace's init");
    alarm(0);
    printf("pidnstest: %s\n", failures ? "FAILED" : "all PID namespace tests passed");
    return failures != 0;
}
