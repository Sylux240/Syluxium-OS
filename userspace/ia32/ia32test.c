// 32-bit (i386) Linux program: built with `gcc -m32 -static`, so it enters
// the kernel through int $0x80 / the vsyscall page with i386 syscall numbers
// and 32-bit structure layouts. Exercises the compat syscall layer.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int passed, failed;
static void check(int ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) passed++; else failed++;
}

static volatile sig_atomic_t got_plain, got_info, info_signo;
static void plain_handler(int sig) { got_plain = sig; }
static void info_handler(int sig, siginfo_t* si, void* uc) {
    (void)uc;
    got_info = sig;
    info_signo = si->si_signo;
}

static sigjmp_buf segv_jump;
static void segv_handler(int sig) { (void)sig; siglongjmp(segv_jump, 1); }

// Raw int $0x80, bypassing glibc entirely.
static long raw_syscall3(long nr, long a, long b, long c) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}

int main(int argc, char** argv) {
    (void)argc;
    struct utsname u;
    uname(&u);
    printf("ia32test: 32-bit pid %d on %s %s, sizeof(void*) = %zu, stack at %p\n", getpid(), u.sysname,
           u.release, sizeof(void*), (void*)&u);

    check(sizeof(void*) == 4 && sizeof(long) == 4, "running as a 32-bit program");
    check((uintptr_t)argv < 0xFFFFFFFFu && argv[0] && strstr(argv[0], "ia32test"), "argv on a 32-bit stack");
    check(raw_syscall3(SYS_getpid, 0, 0, 0) == getpid(), "raw int $0x80 getpid");
    const char msg[] = "  (raw int $0x80 write)\n";
    check(raw_syscall3(SYS_write, 1, (long)msg, sizeof(msg) - 1) == (long)sizeof(msg) - 1, "raw int $0x80 write");

    // Memory: brk via malloc, mmap2, all below 4 GiB.
    char* heap = malloc(100000);
    memset(heap, 0x5a, 100000);
    check(heap && heap[99999] == 0x5a, "malloc 100 KB (brk/mmap2)");
    free(heap);
    char* big = mmap(NULL, 64 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(big != MAP_FAILED, "mmap 64 MiB anonymous");
    if (big != MAP_FAILED) {
        big[0] = 1;
        big[(64 << 20) - 1] = 2;
        check(big[0] + big[(64 << 20) - 1] == 3, "touch both ends of the mapping");
        check(munmap(big, 64 << 20) == 0, "munmap");
    }

    // Files: open/write/lseek/read/stat (stat64 or statx), 64-bit offsets.
    const char* path = "/tmp/ia32-file";
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    check(fd >= 0, "open O_CREAT");
    check(write(fd, "hello world", 11) == 11, "write");
    check(lseek(fd, 6, SEEK_SET) == 6, "lseek");
    char buf[64] = {0};
    check(read(fd, buf, 5) == 5 && memcmp(buf, "world", 5) == 0, "read after lseek");
    check(pread(fd, buf, 5, 0) == 5 && memcmp(buf, "hello", 5) == 0, "pread64");
    off64_t pos = lseek64(fd, 0, SEEK_END);
    check(pos == 11, "_llseek to end");
    struct stat st;
    check(fstat(fd, &st) == 0 && st.st_size == 11 && S_ISREG(st.st_mode), "fstat");
    check(stat(path, &st) == 0 && st.st_size == 11, "stat");
    check(ftruncate(fd, 4) == 0 && fstat(fd, &st) == 0 && st.st_size == 4, "ftruncate64");
    struct iovec iov[2] = {{"ab", 2}, {"cd", 2}};
    lseek(fd, 0, SEEK_SET);
    check(writev(fd, iov, 2) == 4, "writev (32-bit iovec)");
    char r1[2], r2[2];
    struct iovec riov[2] = {{r1, 2}, {r2, 2}};
    lseek(fd, 0, SEEK_SET);
    check(readv(fd, riov, 2) == 4 && r1[0] == 'a' && r2[1] == 'd', "readv");
    close(fd);
    check(unlink(path) == 0 && stat(path, &st) < 0 && errno == ENOENT, "unlink + ENOENT");

    // Directories: getdents64.
    DIR* d = opendir("/usr/bin");
    int entries = 0;
    struct dirent* e;
    while (d && (e = readdir(d)))
        entries++;
    if (d) closedir(d);
    check(entries > 100, "readdir /usr/bin");

    struct statfs sfs;
    check(statfs("/", &sfs) == 0 && sfs.f_bsize > 0, "statfs");

    // Time: clock_gettime(64), gettimeofday, nanosleep.
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct timespec nap = {0, 20 * 1000 * 1000};
    nanosleep(&nap, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    check(ms >= 19 && ms < 1000, "nanosleep 20 ms measured by clock_gettime");
    struct timeval tv;
    check(gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 1600000000, "gettimeofday (wall clock)");
    check(time(NULL) > 1600000000, "time");
    struct sysinfo si;
    check(sysinfo(&si) == 0 && si.totalram > 0 && si.mem_unit > 0, "sysinfo");
    struct rlimit rl;
    check(getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur > 0, "getrlimit");

    // Signals: plain handler (sigreturn) and SA_SIGINFO handler (rt_sigreturn).
    signal(SIGUSR1, plain_handler);
    raise(SIGUSR1);
    check(got_plain == SIGUSR1, "signal handler + sigreturn");
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = info_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR2, &sa, NULL);
    kill(getpid(), SIGUSR2);
    check(got_info == SIGUSR2 && info_signo == SIGUSR2, "SA_SIGINFO handler + rt_sigreturn");
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    got_plain = 0;
    sigprocmask(SIG_BLOCK, &set, &old);
    raise(SIGUSR1);
    int blocked_ok = got_plain == 0;
    sigprocmask(SIG_SETMASK, &old, NULL);
    check(blocked_ok && got_plain == SIGUSR1, "blocked signal delivered on unblock");
    signal(SIGSEGV, segv_handler);
    int caught = 0;
    if (sigsetjmp(segv_jump, 1) == 0)
        *(volatile int*)16 = 1;
    else
        caught = 1;
    check(caught, "SIGSEGV handler + siglongjmp out of it");

    // Processes: fork, pipe, wait, exec of a 64-bit program from a 32-bit one.
    int p[2];
    check(pipe(p) == 0, "pipe");
    pid_t child = fork();
    if (child == 0) {
        close(p[0]);
        write(p[1], "from child", 10);
        _exit(42);
    }
    close(p[1]);
    memset(buf, 0, sizeof(buf));
    check(read(p[0], buf, sizeof(buf)) == 10 && strcmp(buf, "from child") == 0, "fork + pipe");
    int status;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 42, "waitpid status");
    close(p[0]);

    child = fork();
    if (child == 0) {
        char* args[] = {"/opt/syluxium/tests/hello", NULL};
        execv(args[0], args);
        _exit(99);
    }
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "exec a 64-bit program from a 32-bit one");

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(0, &rfds);
    struct timeval zero = {0, 0};
    check(select(1, NULL, NULL, NULL, &zero) == 0, "select with a timeout");

    printf("ia32test: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
