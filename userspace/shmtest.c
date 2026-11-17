// Shared memory: memfd (+seals), anonymous MAP_SHARED, POSIX shm_open,
// System V shm + semaphores, MAP_SHARED tmpfs files, no leaks.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(c, what) do { if (c) printf("  [ ok ] %s\n", what); else { printf("  [FAIL] %s (errno %d %s)\n", what, errno, strerror(errno)); failures++; } } while (0)

static long mem_free_kb(void) {
    FILE* f = fopen("/proc/meminfo", "r");
    char line[128];
    long v = -1;
    while (f && fgets(line, sizeof(line), f))
        if (sscanf(line, "MemFree: %ld kB", &v) == 1) break;
    if (f) fclose(f);
    return v;
}

static int child_status(pid_t pid) { int st; waitpid(pid, &st, 0); return WIFEXITED(st) ? WEXITSTATUS(st) : 99; }

int main(void) {
    printf("shmtest: shared memory\n");

    // memfd: child writes through its own mapping, parent sees it
    int mfd = memfd_create("wl_shm_buffer", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    CHECK(mfd >= 0 && ftruncate(mfd, 8192) == 0, "memfd_create + ftruncate");
    char* a = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    pid_t c = fork();
    if (c == 0) {
        char* b = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
        strcpy(b + 4096, "written by the child");
        _exit(0);
    }
    child_status(c);
    CHECK(strcmp(a + 4096, "written by the child") == 0, "memfd: a child's write appears in the parent's mapping");
    char link[64] = {0}, proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", mfd);
    readlink(proc_path, link, sizeof(link) - 1);
    CHECK(strstr(link, "memfd:wl_shm_buffer") != NULL, "memfd: /proc/self/fd shows memfd:name");
    CHECK(fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) == 0 && fcntl(mfd, F_GET_SEALS) == (F_SEAL_SHRINK | F_SEAL_GROW),
          "memfd: F_ADD_SEALS / F_GET_SEALS");
    CHECK(ftruncate(mfd, 100) == -1 && errno == EPERM, "memfd: a shrink-sealed file can't shrink");
    munmap(a, 8192);
    CHECK(fcntl(mfd, F_ADD_SEALS, F_SEAL_WRITE) == 0 && write(mfd, "x", 1) == -1 && errno == EPERM,
          "memfd: F_SEAL_WRITE blocks writes");
    CHECK(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0) == MAP_FAILED && errno == EPERM,
          "memfd: and writable shared mappings");
    close(mfd);

    // anonymous shared memory across fork
    volatile int* counter = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    *counter = 0;
    pid_t kids[4];
    for (int i = 0; i < 4; i++) {
        kids[i] = fork();
        if (kids[i] == 0) { for (int k = 0; k < 1000; k++) __atomic_add_fetch(counter, 1, __ATOMIC_SEQ_CST); _exit(0); }
    }
    for (int i = 0; i < 4; i++) child_status(kids[i]);
    CHECK(*counter == 4000, "MAP_SHARED|MAP_ANONYMOUS: 4 children x 1000 atomic increments = 4000");

    // POSIX shm_open (glibc uses /dev/shm)
    int pfd = shm_open("/syluxium-test", O_CREAT | O_RDWR | O_EXCL, 0600);
    CHECK(pfd >= 0 && ftruncate(pfd, 4096) == 0, "shm_open creates /dev/shm/syluxium-test");
    c = fork();
    if (c == 0) {
        int f = shm_open("/syluxium-test", O_RDWR, 0);
        char* p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
        strcpy(p, "posix shm");
        _exit(0);
    }
    child_status(c);
    char* pm = mmap(NULL, 4096, PROT_READ, MAP_SHARED, pfd, 0);
    CHECK(pm != MAP_FAILED && strcmp(pm, "posix shm") == 0, "shm_open: data shared between processes");
    CHECK(shm_unlink("/syluxium-test") == 0 && strcmp(pm, "posix shm") == 0, "shm_unlink: mapping stays valid");
    munmap(pm, 4096);
    close(pfd);

    // System V shm + semaphores
    int id = shmget(IPC_PRIVATE, 65536, IPC_CREAT | 0600);
    char* s = shmat(id, NULL, 0);
    int sem = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    CHECK(id >= 0 && s != (void*)-1 && sem >= 0 && semctl(sem, 0, SETVAL, 0) == 0, "shmget/shmat/semget");
    c = fork();
    if (c == 0) {
        strcpy(s + 60000, "sysv shared");
        struct sembuf up = {0, 1, 0};
        semop(sem, &up, 1); // tell the parent
        _exit(0);
    }
    struct sembuf down = {0, -1, 0};
    CHECK(semop(sem, &down, 1) == 0 && strcmp(s + 60000, "sysv shared") == 0,
          "semop blocks until the child posts; the child's write is visible");
    child_status(c);
    struct shmid_ds ds;
    CHECK(shmctl(id, IPC_STAT, &ds) == 0 && ds.shm_segsz == 65536, "shmctl IPC_STAT");
    CHECK(shmctl(id, IPC_RMID, NULL) == 0 && strcmp(s + 60000, "sysv shared") == 0, "IPC_RMID while attached: still usable");
    CHECK(shmdt(s) == 0 && shmat(id, NULL, 0) == (void*)-1, "shmdt; the removed segment is gone");
    struct sembuf nowait = {0, -1, IPC_NOWAIT};
    CHECK(semop(sem, &nowait, 1) == -1 && errno == EAGAIN, "semop IPC_NOWAIT -> EAGAIN");
    CHECK(semctl(sem, 0, IPC_RMID) == 0, "semctl IPC_RMID");

    // MAP_SHARED of a tmpfs file + unlink while mapped
    int tf = open("/tmp/shared.bin", O_CREAT | O_RDWR | O_TRUNC, 0600);
    ftruncate(tf, 4096);
    char* t = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, tf, 0);
    strcpy(t, "via mmap");
    char buf[16] = {0};
    pread(tf, buf, 8, 0);
    CHECK(strcmp(buf, "via mmap") == 0, "/tmp file: mmap writes visible to read()");
    pwrite(tf, "VIA", 3, 0);
    CHECK(memcmp(t, "VIA mmap", 8) == 0, "/tmp file: write() visible in the mapping");
    unlink("/tmp/shared.bin");
    close(tf);
    CHECK(memcmp(t, "VIA mmap", 8) == 0, "unlinked + closed: the mapping keeps the file alive");
    munmap(t, 4096);

    // no leaks: create/map/free many times
    long before = mem_free_kb();
    for (int i = 0; i < 200; i++) {
        int f = memfd_create("leak", 0);
        ftruncate(f, 64 * 1024);
        char* m = mmap(NULL, 64 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
        memset(m, i, 64 * 1024);
        munmap(m, 64 * 1024);
        close(f);
    }
    long after = mem_free_kb();
    printf("  MemFree before %ld kB, after %ld kB (200 x 64 KiB memfds)\n", before, after);
    CHECK(before - after < 2048, "memfds are freed after close + munmap (no 12.5 MiB leak)");

    printf("shmtest: %s (%d failure%s)\n", failures ? "FAILED" : "all passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
