// Pipe and file-descriptor stress test, for use-after-free bugs in the pipe
// and fd-table code (the kernel heap checks freed objects for writes).
//
//   pipestress [seconds]
//
//  1. close race: pairs of threads close the two ends of a fresh pipe at
//     the same instant (both ends' releases run concurrently).
//  2. dup storm: dup/dup2/close of pipe ends from several threads while
//     others read and write through them.
//  3. fork/exec: children inherit pipes, write and exit, while the parent
//     keeps making and closing pipes.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

static int seconds = 3;
static atomic_int stop;
static atomic_long rounds;

// ---- 1. both ends closed at once ----
struct racer {
    atomic_int go, done;
    int fd;
};
static struct racer racers[4];

static void* closer(void* arg) {
    struct racer* r = arg;
    while (!atomic_load(&stop)) {
        while (atomic_load(&r->go) == 0)
            if (atomic_load(&stop))
                return NULL;
        close(r->fd);
        atomic_store(&r->go, 0);
        atomic_fetch_add(&r->done, 1);
    }
    return NULL;
}

static int test_close_race(void) {
    pthread_t t[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, closer, &racers[i]);
    long n = 0;
    while (!atomic_load(&stop)) {
        int a[2], b[2];
        if (pipe2(a, n & 1 ? O_NONBLOCK : 0) < 0 || pipe(b) < 0)
            break;
        if (n & 2) { // something buffered, and a reader that is woken by the close
            if (write(a[1], "x", 1) < 0)
                break;
        }
        racers[0].fd = a[0], racers[1].fd = a[1], racers[2].fd = b[0], racers[3].fd = b[1];
        int before[4];
        for (int i = 0; i < 4; i++)
            before[i] = atomic_load(&racers[i].done);
        for (int i = 0; i < 4; i++)
            atomic_store(&racers[i].go, 1);
        for (int i = 0; i < 4; i++)
            while (atomic_load(&racers[i].done) == before[i] && !atomic_load(&stop))
                ;
        n++;
    }
    atomic_store(&stop, 1);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    printf("  [PASS] close race: %ld pipe pairs closed from both ends at once\n", n);
    return 0;
}

// ---- 2. dup storm ----
static int shared[2];

static void* dupper(void* arg) {
    (void)arg;
    while (!atomic_load(&stop)) {
        int d = dup(shared[(rounds & 1)]);
        if (d >= 0) {
            int on = 1;
            ioctl(d, FIONBIO, &on);
            int e = dup2(d, d + 50);
            close(d);
            if (e >= 0)
                close(e);
        }
        atomic_fetch_add(&rounds, 1);
    }
    return NULL;
}

static void* rw(void* arg) {
    long w = (long)arg;
    char buf[64];
    while (!atomic_load(&stop)) {
        if (w)
            (void)!write(shared[1], "hello pipe", 10);
        else
            (void)!read(shared[0], buf, sizeof(buf));
    }
    return NULL;
}

static int test_dup_storm(void) {
    if (pipe2(shared, O_NONBLOCK) < 0)
        return 1;
    pthread_t t[6];
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, dupper, NULL);
    pthread_create(&t[4], NULL, rw, (void*)0);
    pthread_create(&t[5], NULL, rw, (void*)1);
    sleep(seconds);
    atomic_store(&stop, 1);
    for (int i = 0; i < 6; i++)
        pthread_join(t[i], NULL);
    close(shared[0]);
    close(shared[1]);
    printf("  [PASS] dup storm: %ld dup/dup2/close rounds\n", atomic_load(&rounds));
    return 0;
}

// ---- 3. fork/exec with inherited pipes ----
static int test_fork(void) {
    long kids = 0;
    int bad = 0;
    while (!atomic_load(&stop)) {
        int p[2];
        if (pipe(p) < 0)
            return 1;
        pid_t pid = fork();
        if (pid == 0) {
            close(p[0]);
            dup2(p[1], 1);
            execl("/bin/echo", "echo", "child", (char*)NULL);
            _exit(127);
        }
        close(p[1]);
        char buf[16];
        ssize_t n = read(p[0], buf, sizeof(buf));
        close(p[0]);
        int st;
        waitpid(pid, &st, 0);
        if (n != 6 || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
            bad++;
        // Churn: pipes made and dropped while the child runs.
        for (int i = 0; i < 8; i++) {
            int q[2];
            if (pipe(q) == 0) {
                close(q[1]);
                close(q[0]);
            }
        }
        kids++;
    }
    printf("  [%s] fork/exec with pipes: %ld children, %d bad\n", bad ? "FAIL" : "PASS", kids, bad);
    return bad != 0;
}

static void* timer(void* arg) {
    (void)arg;
    sleep(seconds);
    atomic_store(&stop, 1);
    return NULL;
}

int main(int argc, char** argv) {
    if (argc > 1)
        seconds = atoi(argv[1]);
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("pipestress: %d s per test\n", seconds);
    pthread_t tm;
    int fails = 0;
    pthread_create(&tm, NULL, timer, NULL);
    fails += test_close_race();
    pthread_join(tm, NULL);
    atomic_store(&stop, 0);
    fails += test_dup_storm();
    atomic_store(&stop, 0);
    pthread_create(&tm, NULL, timer, NULL);
    fails += test_fork();
    pthread_join(tm, NULL);
    printf("pipestress: %s\n", fails ? "FAILED" : "all passed");
    return fails;
}
