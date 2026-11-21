// GPU buffer mapping churn: what Mesa does all the time (map, unmap, create,
// destroy). Every mapping must be released again, or mmap() runs out and GL
// reports GL_OUT_OF_MEMORY.
//   drmmaptest [/dev/dri/cardN]
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct map_dumb { uint32_t handle, pad; uint64_t offset; };
struct destroy_dumb { uint32_t handle; };
#define IOCTL_CREATE_DUMB _IOWR('d', 0xB2, struct create_dumb)
#define IOCTL_MAP_DUMB _IOWR('d', 0xB3, struct map_dumb)
#define IOCTL_DESTROY_DUMB _IOWR('d', 0xB4, struct destroy_dumb)

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "/dev/dri/card0";
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        printf("drmmaptest: %s: %s\n", path, strerror(errno));
        return 1;
    }
    int failures = 0;
    // 1. one buffer, mapped and unmapped 10000 times
    struct create_dumb c = {.height = 64, .width = 64, .bpp = 32};
    if (ioctl(fd, IOCTL_CREATE_DUMB, &c)) {
        printf("drmmaptest: CREATE_DUMB: %s\n", strerror(errno));
        return 1;
    }
    struct map_dumb m = {.handle = c.handle};
    if (ioctl(fd, IOCTL_MAP_DUMB, &m)) {
        printf("drmmaptest: MAP_DUMB: %s\n", strerror(errno));
        return 1;
    }
    for (int i = 0; i < 10000; i++) {
        uint32_t* p = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)m.offset);
        if (p == MAP_FAILED) {
            printf("drmmaptest: mmap #%d failed: %s\n", i, strerror(errno));
            failures++;
            break;
        }
        p[i % 1024] = (uint32_t)i;
        munmap(p, c.size);
    }
    struct destroy_dumb d = {.handle = c.handle};
    ioctl(fd, IOCTL_DESTROY_DUMB, &d);
    // 2. 3000 buffers created, mapped, touched, unmapped and destroyed
    for (int i = 0; i < 3000 && !failures; i++) {
        struct create_dumb b = {.height = 16, .width = 64, .bpp = 32};
        if (ioctl(fd, IOCTL_CREATE_DUMB, &b)) {
            printf("drmmaptest: CREATE_DUMB #%d: %s\n", i, strerror(errno));
            failures++;
            break;
        }
        struct map_dumb bm = {.handle = b.handle};
        if (ioctl(fd, IOCTL_MAP_DUMB, &bm)) {
            printf("drmmaptest: MAP_DUMB #%d: %s\n", i, strerror(errno));
            failures++;
            break;
        }
        uint32_t* p = mmap(NULL, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)bm.offset);
        if (p == MAP_FAILED) {
            printf("drmmaptest: mmap of buffer #%d failed: %s\n", i, strerror(errno));
            failures++;
            break;
        }
        p[0] = 0x12345678;
        munmap(p, b.size);
        struct destroy_dumb bd = {.handle = b.handle};
        ioctl(fd, IOCTL_DESTROY_DUMB, &bd);
    }
    printf("drmmaptest: %s\n", failures ? "FAILED" : "10000 mappings of one buffer and 3000 buffers: all released");
    return failures != 0;
}
