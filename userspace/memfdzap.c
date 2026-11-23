// Stress: hole punching in a memfd (Android's ashmem purge) while other
// processes map it (private and shared), touch it, write to it and exit.
// The kernel unmaps punched pages from every mapping; that must not race
// the mappers' own exit or munmap (double page release, freed page tables).
//   memfdzap [seconds]   -> "memfdzap: ok" (the kernel log shows any bad release)
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PAGES 64
#define SIZE (PAGES * 4096)

static void mapper(int fd, unsigned seed) {
    for (int round = 0; round < 50; round++) {
        int priv = (seed + round) & 1;
        unsigned char* m = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, priv ? MAP_PRIVATE : MAP_SHARED, fd, 0);
        if (m == MAP_FAILED)
            _exit(2);
        for (int i = 0; i < PAGES; i++) {
            volatile unsigned char x = m[i * 4096];
            (void)x;
            if ((i + round) % 3 == 0)
                m[i * 4096 + 1] = (unsigned char)i; // copy-on-write (private) or a shared write
        }
        if (round % 7 == 0)
            _exit(0); // exit with the mapping still there
        munmap(m, SIZE);
    }
    _exit(0);
}

int main(int argc, char** argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 20;
    int fd = memfd_create("memfdzap", 0);
    if (fd < 0 || ftruncate(fd, SIZE) < 0) {
        perror("memfdzap");
        return 2;
    }
    time_t end = time(NULL) + seconds;
    long punches = 0, children = 0;
    unsigned seed = 1;
    while (time(NULL) < end) {
        // Keep a few mappers running.
        for (int k = 0; k < 4; k++) {
            pid_t pid = fork();
            if (pid == 0)
                mapper(fd, seed + k);
            if (pid > 0)
                children++;
        }
        seed += 4;
        // Meanwhile: punch holes (and refill them by writing).
        for (int r = 0; r < 200; r++) {
            int page = (r * 13 + seed) % PAGES;
            fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)page * 4096, 4096);
            punches++;
            if (r % 5 == 0) {
                char b = 'x';
                pwrite(fd, &b, 1, (off_t)((page + 1) % PAGES) * 4096);
            }
        }
        while (waitpid(-1, NULL, WNOHANG) > 0) {}
    }
    while (wait(NULL) > 0) {}
    printf("memfdzap: ok (%ld punches, %ld processes)\n", punches, children);
    return 0;
}
