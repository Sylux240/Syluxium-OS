// Test: a file mapped, closed and deleted keeps its contents for the
// mapping (a running program whose binary apt replaces). Afterwards other
// files reuse the freed blocks; the mapping must still read the original.
//   mapunlink [dir]     (default /var/tmp: the disk, not the RAM root)
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SIZE (256 * 4096)

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "/var/tmp";
    char path[256], other[256];
    snprintf(path, sizeof(path), "%s/mapunlink.%d", dir, getpid());
    snprintf(other, sizeof(other), "%s/mapunlink-fill.%d", dir, getpid());
    unsigned char* buf = malloc(SIZE);
    for (int i = 0; i < SIZE; i++)
        buf[i] = (unsigned char)(i * 7 + i / 4096);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, buf, SIZE) != SIZE) {
        perror("mapunlink: create");
        return 2;
    }
    fsync(fd);
    unsigned char* m = mmap(NULL, SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) {
        perror("mapunlink: mmap");
        return 2;
    }
    volatile unsigned char touch = m[0]; // only the first page faulted in now
    (void)touch;
    close(fd);
    unlink(path);
    // Reuse the freed blocks (and inode) with different data.
    for (int round = 0; round < 4; round++) {
        int f = open(other, O_RDWR | O_CREAT | O_TRUNC, 0644);
        memset(buf, 0xA5 + round, SIZE);
        if (f >= 0) {
            if (write(f, buf, SIZE) != SIZE)
                perror("mapunlink: fill");
            fsync(f);
            close(f);
        }
        unlink(other);
    }
    int bad = 0;
    for (int i = 0; i < SIZE; i++) {
        unsigned char want = (unsigned char)(i * 7 + i / 4096);
        if (m[i] != want) {
            if (bad++ < 3)
                printf("mapunlink: byte %d is %#x, want %#x\n", i, m[i], want);
        }
    }
    munmap(m, SIZE);
    if (bad) {
        printf("mapunlink: FAIL (%d bytes wrong)\n", bad);
        return 1;
    }
    printf("mapunlink: ok\n");
    return 0;
}
