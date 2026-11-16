// Exercises the Linux syscall layer through ordinary glibc calls.
// Prints one line per check and exits 0 only if everything passed.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static int passed, failed;

static void check(int ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (ok)
        passed++;
    else
        failed++;
}

static double elapsed_ms(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6;
}

int main(int argc, char** argv) {
    printf("systest: pid %d, argc %d\n", getpid(), argc);

    // argv / environment passed through the initial stack
    check(argc == 3 && strcmp(argv[1], "arg-one") == 0 && strcmp(argv[2], "arg two") == 0, "argv");
    const char* path = getenv("PATH");
    check(path && strstr(path, "/bin"), "environment (PATH)");

    struct utsname u;
    check(uname(&u) == 0 && strcmp(u.machine, "x86_64") == 0, "uname");

    // Files: create, write, read back, seek, stat, append, unlink
    mkdir("/tmp/systest", 0755);
    int fd = open("/tmp/systest/file.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    const char msg[] = "Syluxium writes files!";
    check(fd >= 0 && write(fd, msg, sizeof(msg)) == (ssize_t)sizeof(msg), "open(O_CREAT) + write");
    char buf[64] = {0};
    check(lseek(fd, 0, SEEK_SET) == 0 && read(fd, buf, sizeof(buf)) == (ssize_t)sizeof(msg) &&
              strcmp(buf, msg) == 0, "lseek + read back");
    struct stat st;
    check(fstat(fd, &st) == 0 && st.st_size == (off_t)sizeof(msg) && S_ISREG(st.st_mode), "fstat");
    close(fd);

    FILE* f = fopen("/tmp/systest/file.txt", "a");
    fprintf(f, "%s", " And appends.");
    fclose(f);
    check(stat("/tmp/systest/file.txt", &st) == 0 && st.st_size == (off_t)(sizeof(msg) + 13), "stdio append");
    check(unlink("/tmp/systest/file.txt") == 0 && access("/tmp/systest/file.txt", F_OK) != 0 && errno == ENOENT,
          "unlink");

    // Directories
    DIR* d = opendir("/bin");
    int entries = 0, saw_hello = 0;
    struct dirent* e;
    while (d && (e = readdir(d))) {
        entries++;
        if (strcmp(e->d_name, "hello") == 0)
            saw_hello = 1;
    }
    if (d)
        closedir(d);
    check(saw_hello && entries >= 4, "opendir/readdir /bin");

    char cwd[256];
    check(chdir("/tmp") == 0 && getcwd(cwd, sizeof(cwd)) && strcmp(cwd, "/tmp") == 0, "chdir + getcwd");

    // Memory: small (brk) and large (mmap) allocations
    int heap_ok = 1;
    for (int i = 0; i < 1000; i++) {
        char* p = malloc(64 + i);
        if (!p) { heap_ok = 0; break; }
        memset(p, i, 64 + i);
        free(p);
    }
    char* big = malloc(8 << 20);
    if (big) {
        for (size_t i = 0; i < (8u << 20); i += 4096)
            big[i] = (char)i;
        for (size_t i = 0; i < (8u << 20); i += 4096)
            if (big[i] != (char)i) heap_ok = 0;
        free(big);
    } else {
        heap_ok = 0;
    }
    check(heap_ok, "malloc: 1000 small + one 8 MiB");

    char* m = mmap(NULL, 1 << 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int mm_ok = m != MAP_FAILED;
    if (mm_ok) {
        m[0] = 1;
        m[65535] = 2;
        mm_ok = mprotect(m, 1 << 16, PROT_READ) == 0 && m[0] == 1 && m[65535] == 2 && munmap(m, 1 << 16) == 0;
    }
    check(mm_ok, "mmap / mprotect / munmap");

    // Time
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct timespec nap = {0, 50 * 1000 * 1000};
    nanosleep(&nap, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = elapsed_ms(t0, t1);
    check(ms >= 50 && ms < 1000, "nanosleep(50 ms) measured with CLOCK_MONOTONIC");

    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    char date[64];
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S UTC", &tm);
    printf("  wall clock: %s\n", date);
    check(tm.tm_year + 1900 >= 2024, "wall clock from the RTC");

    // Randomness and devices
    unsigned char r1[16] = {0}, r2[16] = {0};
    check(getrandom(r1, sizeof(r1), 0) == 16 && getrandom(r2, sizeof(r2), 0) == 16 && memcmp(r1, r2, 16) != 0,
          "getrandom");
    int zfd = open("/dev/zero", O_RDONLY);
    char z[32];
    memset(z, 0x55, sizeof(z));
    check(zfd >= 0 && read(zfd, z, sizeof(z)) == 32 && z[0] == 0 && z[31] == 0, "/dev/zero");
    close(zfd);
    check(isatty(1), "stdout is a terminal (TCGETS)");

    // Floating point through libm and SSE/AVX code paths in glibc
    double s = 0;
    for (int i = 1; i <= 1000; i++)
        s += sqrt((double)i) * sin(i * 0.001);
    char fbuf[32];
    snprintf(fbuf, sizeof(fbuf), "%.6f", s);
    printf("  float checksum: %s\n", fbuf);
    check(fabs(s - 11531.016122) < 1e-4, "floating point math (matches the host result)");

    printf("systest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
