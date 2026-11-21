// A file deleted while open must keep its data until the last close (and
// other files must not get its blocks meanwhile).
//
//   orphantest [dir] [hold]
//
// hold: keep the deleted file open and sleep (to test what the next mount
// does with an orphan after a power cut).
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;
static void check(int ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        fails++;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "/root";
    int hold = argc > 2 && strcmp(argv[2], "hold") == 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    char path[256], other[256];
    snprintf(path, sizeof(path), "%s/orphantest.%d", dir, getpid());
    snprintf(other, sizeof(other), "%s/orphantest-other.%d", dir, getpid());
    enum { SIZE = 4 << 20 };
    char* data = malloc(SIZE);
    char* back = malloc(SIZE);
    for (int i = 0; i < SIZE; i++)
        data[i] = (char)(i * 31 + i / 4096);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0 && write(fd, data, SIZE) == SIZE, "write 4 MiB");
    check(unlink(path) == 0 && access(path, F_OK) != 0, "unlink it while open");
    // Other files written now must not land on its blocks.
    int o = open(other, O_RDWR | O_CREAT | O_TRUNC, 0600);
    char* junk = malloc(SIZE);
    memset(junk, 0x5A, SIZE);
    check(o >= 0 && write(o, junk, SIZE) == SIZE, "write another 4 MiB file");
    close(o);
    unlink(other);
    check(pread(fd, back, SIZE, 0) == SIZE && memcmp(back, data, SIZE) == 0, "deleted file still reads back intact");
    check(pwrite(fd, data, 65536, SIZE) == 65536, "deleted file still grows");
    check(pread(fd, back, 65536, SIZE) == 65536 && memcmp(back, data, 65536) == 0, "and reads its new data");
    if (hold) {
        printf("ORPHAN HELD\n");
        for (;;)
            pause();
    }
    close(fd);
    printf("orphantest: %d failed\n", fails);
    return fails;
}
