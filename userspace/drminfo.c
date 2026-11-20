// drminfo: what drives /dev/dri/cardN (DRM_IOCTL_VERSION), for
// syluxium-display-config and hardware reports.
//   drminfo [card]            prints "<driver> <major>.<minor>.<patch> <description>"
//   drminfo -n [card]         prints only the driver name
//   drminfo -t [card] [count] GEM dumb-buffer test: create, mmap, write,
//                             read back, munmap, destroy (count times)
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct drm_version {
    int version_major, version_minor, version_patchlevel;
    size_t name_len; char* name;
    size_t date_len; char* date;
    size_t desc_len; char* desc;
};
struct drm_mode_create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct drm_mode_map_dumb { uint32_t handle, pad; uint64_t offset; };
struct drm_mode_destroy_dumb { uint32_t handle; };

#define DRM_IOCTL_VERSION _IOWR('d', 0x00, struct drm_version)
#define DRM_IOCTL_MODE_CREATE_DUMB _IOWR('d', 0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB _IOWR('d', 0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB _IOWR('d', 0xB4, struct drm_mode_destroy_dumb)

static int dumb_test(int fd, int count) {
    for (int i = 0; i < count; i++) {
        struct drm_mode_create_dumb c = {.height = 256, .width = 256, .bpp = 32};
        if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) { perror("create_dumb"); return 1; }
        struct drm_mode_map_dumb m = {.handle = c.handle};
        if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) { perror("map_dumb"); return 1; }
        uint32_t* p = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
        if (p == MAP_FAILED) { perror("mmap"); return 1; }
        size_t n = c.size / 4;
        for (size_t j = 0; j < n; j++) p[j] = (uint32_t)(j * 2654435761u + i);
        for (size_t j = 0; j < n; j++)
            if (p[j] != (uint32_t)(j * 2654435761u + i)) { fprintf(stderr, "mismatch at %zu\n", j); return 1; }
        munmap(p, c.size);
        struct drm_mode_destroy_dumb d = {.handle = c.handle};
        if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d)) { perror("destroy_dumb"); return 1; }
    }
    printf("dumb buffer test: %d x %u KiB OK\n", count, 256 * 256 * 4 / 1024);
    return 0;
}

int main(int argc, char** argv) {
    int name_only = 0, test = 0, count = 100;
    int a = 1;
    if (a < argc && !strcmp(argv[a], "-n")) { name_only = 1; a++; }
    else if (a < argc && !strcmp(argv[a], "-t")) { test = 1; a++; }
    const char* card = a < argc ? argv[a++] : "/dev/dri/card0";
    if (a < argc) count = atoi(argv[a]);
    int fd = open(card, O_RDWR | O_CLOEXEC);
    if (fd < 0) { fprintf(stderr, "%s: %s\n", card, strerror(errno)); return 1; }
    if (test) return dumb_test(fd, count);
    char name[64] = "", date[64] = "", desc[256] = "";
    struct drm_version v = {.name_len = sizeof(name) - 1, .name = name, .date_len = sizeof(date) - 1, .date = date,
                            .desc_len = sizeof(desc) - 1, .desc = desc};
    if (ioctl(fd, DRM_IOCTL_VERSION, &v)) { perror("DRM_IOCTL_VERSION"); return 1; }
    if (name_only) printf("%s\n", name);
    else printf("%s %d.%d.%d %s\n", name, v.version_major, v.version_minor, v.version_patchlevel, desc);
    return 0;
}
