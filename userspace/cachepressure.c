// Test: the page cache gives memory back. Maps and touches every file under
// a directory (fills the cache with pages no mapping keeps), then allocates
// and touches more anonymous memory than is free: it must succeed, with the
// kernel reclaiming the cache (/proc/meminfo Cached drops).
//   cachepressure <dir> <MiB to allocate>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static long meminfo(const char* key) {
    FILE* f = fopen("/proc/meminfo", "r");
    char line[128];
    long v = -1;
    size_t n = strlen(key);
    while (f && fgets(line, sizeof(line), f))
        if (!strncmp(line, key, n) && line[n] == ':')
            v = atol(line + n + 1);
    if (f)
        fclose(f);
    return v;
}

static long touched;

static void walk(const char* dir, int depth) {
    DIR* d = opendir(dir);
    if (!d || depth > 6)
        return;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(path, &st) < 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            walk(path, depth + 1);
        } else if (S_ISREG(st.st_mode) && st.st_size >= 4096) {
            int fd = open(path, O_RDONLY);
            if (fd < 0)
                continue;
            unsigned char* m = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            close(fd);
            if (m == MAP_FAILED)
                continue;
            volatile unsigned char sum = 0;
            for (off_t o = 0; o + 4096 <= st.st_size; o += 4096) {
                sum += m[o];
                touched++;
            }
            munmap(m, st.st_size);
        }
    }
    closedir(d);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: cachepressure <dir> <MiB>\n");
        return 2;
    }
    walk(argv[1], 0);
    printf("cachepressure: touched %ld file pages; MemFree %ld kB, Cached %ld kB, MemAvailable %ld kB\n", touched,
           meminfo("MemFree"), meminfo("Cached"), meminfo("MemAvailable"));
    long mib = atol(argv[2]);
    size_t size = (size_t)mib << 20;
    unsigned char* a = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a == MAP_FAILED) {
        perror("cachepressure: mmap");
        return 1;
    }
    for (size_t o = 0; o < size; o += 4096)
        a[o] = 1; // SIGSEGV here if the kernel can't find memory
    printf("cachepressure: allocated %ld MiB; MemFree %ld kB, Cached %ld kB\n", mib, meminfo("MemFree"),
           meminfo("Cached"));
    printf("cachepressure: ok\n");
    return 0;
}
