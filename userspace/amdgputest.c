// amdgputest: the first user-space work on an AMD GPU, one step at a time,
// each step written to a log file and synced to disk before it runs, so a
// machine that freezes leaves the step it froze in.
//   amdgputest [render node] [log file]
// Steps, safest first: context; GTT buffer created, mapped, written, read
// back by the CPU; the same for a VRAM buffer (through the BAR); both mapped
// into the process's GPU address space; then each engine (SDMA, GFX,
// compute) writes a value into the GTT buffer and the VRAM buffer from an
// IB in the process's own address space (its own VMID and page tables, not
// the kernel's GART), and the CPU checks it. Raw ioctls (no libdrm).
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

struct drm_version {
    int version_major, version_minor, version_patchlevel;
    size_t name_len; char* name;
    size_t date_len; char* date;
    size_t desc_len; char* desc;
};
union gem_create { struct { uint64_t bo_size, alignment, domains, domain_flags; } in; struct { uint32_t handle, pad; } out; };
union gem_mmap { struct { uint32_t handle, pad; } in; struct { uint64_t addr_ptr; } out; };
union ctx { struct { uint32_t op, flags, ctx_id; int32_t priority; } in; struct { uint32_t ctx_id, pad; } out; };
struct gem_va { uint32_t handle, pad, operation, flags; uint64_t va_address, offset_in_bo, map_size; };
struct cs_chunk { uint32_t chunk_id, length_dw; uint64_t chunk_data; };
struct cs_chunk_ib { uint32_t pad, flags; uint64_t va_start; uint32_t ib_bytes, ip_type, ip_instance, ring; };
struct bo_list_in { uint32_t operation, list_handle, bo_number, bo_info_size; uint64_t bo_info_ptr; };
struct bo_list_entry { uint32_t bo_handle, bo_priority; };
union cs { struct { uint32_t ctx_id, bo_list_handle, num_chunks, flags; uint64_t chunks; } in; struct { uint64_t handle; } out; };
union wait_cs { struct { uint64_t handle, timeout; uint32_t ip_type, ip_instance, ring, ctx_id; } in; struct { uint64_t status; } out; };

#define DRM_IOCTL_VERSION _IOWR('d', 0x00, struct drm_version)
#define AMDGPU_GEM_CREATE _IOWR('d', 0x40, union gem_create)
#define AMDGPU_GEM_MMAP _IOWR('d', 0x41, union gem_mmap)
#define AMDGPU_CTX _IOWR('d', 0x42, union ctx)
#define AMDGPU_CS _IOWR('d', 0x44, union cs)
#define AMDGPU_GEM_VA _IOW('d', 0x48, struct gem_va)
#define AMDGPU_WAIT_CS _IOWR('d', 0x49, union wait_cs)

#define DOMAIN_GTT 2
#define DOMAIN_VRAM 4
#define FLAG_CPU_ACCESS 1
#define FLAG_GTT_USWC 4
#define IP_GFX 0
#define IP_COMPUTE 1
#define IP_DMA 2

static FILE* logf;
static struct timespec t0;

static void say(const char* fmt, ...) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    double s = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("[%6.3f] %s\n", s, line);
    fflush(stdout);
    if (logf) {
        fprintf(logf, "[%6.3f] %s\n", s, line);
        fflush(logf);
        fsync(fileno(logf));
    }
    // Into the kernel log too, between the driver's own lines.
    int k = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (k >= 0) {
        dprintf(k, "amdgputest: %s\n", line);
        close(k);
    }
}

static int fd;

struct bo { uint32_t handle; uint64_t size, va; uint32_t* cpu; const char* name; };

static int bo_create(struct bo* b, const char* name, uint64_t size, uint64_t domains, uint64_t flags, int map) {
    b->name = name;
    b->size = size;
    say("step: create %s buffer (%llu KiB, domains %#llx, flags %#llx)", name, (unsigned long long)(size >> 10),
        (unsigned long long)domains, (unsigned long long)flags);
    union gem_create c = {.in = {size, 4096, domains, flags}};
    if (ioctl(fd, AMDGPU_GEM_CREATE, &c)) { say("FAIL: GEM_CREATE: %s", strerror(errno)); return -1; }
    b->handle = c.out.handle;
    if (!map) return 0;
    say("step: map %s buffer for the CPU", name);
    union gem_mmap m = {.in = {b->handle, 0}};
    if (ioctl(fd, AMDGPU_GEM_MMAP, &m)) { say("FAIL: GEM_MMAP: %s", strerror(errno)); return -1; }
    say("  GEM_MMAP returned offset %#llx; now mmap()", (unsigned long long)m.out.addr_ptr);
    b->cpu = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.out.addr_ptr);
    if (b->cpu == MAP_FAILED) { say("FAIL: mmap: %s", strerror(errno)); b->cpu = NULL; return -1; }
    say("  mmap() returned %p", (void*)b->cpu);
    say("step: CPU writes and reads back the %s buffer (first touch: page fault into TTM)", name);
    for (uint64_t i = 0; i < size / 4; i++) b->cpu[i] = 0x11110000u + (uint32_t)i;
    for (uint64_t i = 0; i < size / 4; i++)
        if (b->cpu[i] != 0x11110000u + (uint32_t)i) {
            say("FAIL: %s word %llu reads %#x", name, (unsigned long long)i, b->cpu[i]);
            return -1;
        }
    say("ok: %s buffer CPU access", name);
    return 0;
}

static int bo_va(struct bo* b, uint64_t va) {
    say("step: map %s buffer into the process's GPU address space at %#llx", b->name, (unsigned long long)va);
    struct gem_va v = {.handle = b->handle, .operation = 1, .flags = 2 | 4 | 8, .va_address = va, .map_size = b->size};
    if (ioctl(fd, AMDGPU_GEM_VA, &v)) { say("FAIL: GEM_VA: %s", strerror(errno)); return -1; }
    b->va = va;
    return 0;
}

static uint32_t ctx_id;

// One IB from the ib buffer, on ip; then wait for it (2 s).
static int submit(struct bo* ib, uint32_t ndw, int ip, struct bo** bos, int nbos, const char* what) {
    struct bo_list_entry e[4];
    for (int i = 0; i < nbos; i++) e[i] = (struct bo_list_entry){bos[i]->handle, 0};
    struct bo_list_in bl = {.operation = ~0u, .bo_number = (uint32_t)nbos, .bo_info_size = sizeof(e[0]),
                            .bo_info_ptr = (uintptr_t)e};
    struct cs_chunk_ib cib = {.va_start = ib->va, .ib_bytes = ndw * 4, .ip_type = (uint32_t)ip};
    struct cs_chunk chunks[2] = {{0x06, sizeof(bl) / 4, (uintptr_t)&bl}, {0x01, sizeof(cib) / 4, (uintptr_t)&cib}};
    uint64_t ptrs[2] = {(uintptr_t)&chunks[0], (uintptr_t)&chunks[1]};
    say("step: submit %s (%u dwords)", what, ndw);
    union cs c = {.in = {.ctx_id = ctx_id, .num_chunks = 2, .chunks = (uintptr_t)ptrs}};
    if (ioctl(fd, AMDGPU_CS, &c)) { say("FAIL: CS: %s", strerror(errno)); return -1; }
    say("step: wait for %s (fence %llu)", what, (unsigned long long)c.out.handle);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    union wait_cs w = {.in = {.handle = c.out.handle, .timeout = (uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec + 2000000000ull,
                              .ip_type = (uint32_t)ip, .ctx_id = ctx_id}};
    if (ioctl(fd, AMDGPU_WAIT_CS, &w)) { say("FAIL: WAIT_CS: %s", strerror(errno)); return -1; }
    if (w.out.status) { say("FAIL: %s did not finish in 2 s", what); return -1; }
    return 0;
}

#define PACKET3(op, n) ((3u << 30) | (((n) & 0x3FFF) << 16) | (((op) & 0xFF) << 8))

// Engine writes 0xC0DE00nn into dst at byte offset off.
static int engine_write(int ip, struct bo* ib, struct bo* dst, uint32_t off, uint32_t value, const char* what) {
    uint64_t a = dst->va + off;
    uint32_t n = 0;
    uint32_t* p = ib->cpu;
    if (ip == IP_DMA) {
        p[n++] = 0x00000002; // SDMA WRITE, linear
        p[n++] = (uint32_t)a;
        p[n++] = (uint32_t)(a >> 32);
        p[n++] = 0; // one dword
        p[n++] = value;
        while (n % 8) p[n++] = 0; // SDMA NOP
    } else {
        p[n++] = PACKET3(0x37, 3); // WRITE_DATA
        p[n++] = (5u << 8) | (1u << 20); // to memory, confirm
        p[n++] = (uint32_t)a;
        p[n++] = (uint32_t)(a >> 32);
        p[n++] = value;
        while (n % 8) p[n++] = 0xffff1000; // type-2 NOP
    }
    if (dst->cpu) dst->cpu[off / 4] = 0;
    __sync_synchronize();
    struct bo* bos[2] = {ib, dst};
    if (submit(ib, n, ip, bos, 2, what)) return -1;
    if (!dst->cpu) return 0;
    uint32_t got = dst->cpu[off / 4];
    if (got != value) { say("FAIL: %s: the CPU reads %#x, expected %#x", what, got, value); return -1; }
    say("ok: %s", what);
    return 0;
}

int main(int argc, char** argv) {
    const char* node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (argc > 2) logf = fopen(argv[2], "w");
    fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) { say("FAIL: %s: %s", node, strerror(errno)); return 1; }
    char name[32] = "";
    struct drm_version v = {.name_len = sizeof(name) - 1, .name = name};
    if (ioctl(fd, DRM_IOCTL_VERSION, &v) || strcmp(name, "amdgpu")) {
        say("%s is %s, not amdgpu: nothing to test", node, name[0] ? name : "?");
        return 0;
    }
    say("amdgpu on %s: first user-space work, step by step", node);
    say("step: create a context");
    union ctx c = {.in = {.op = 1}};
    if (ioctl(fd, AMDGPU_CTX, &c)) { say("FAIL: CTX: %s", strerror(errno)); return 1; }
    ctx_id = c.out.ctx_id;

    struct bo gtt = {0}, vram = {0}, ib = {0}, hidden = {0};
    if (bo_create(&gtt, "GTT", 4096, DOMAIN_GTT, 0, 1)) return 1;
    if (bo_create(&ib, "IB (GTT, write-combined)", 4096, DOMAIN_GTT, FLAG_GTT_USWC, 1)) return 1;
    if (bo_create(&vram, "VRAM (CPU-visible)", 65536, DOMAIN_VRAM, FLAG_CPU_ACCESS, 1)) return 1;
    if (bo_create(&hidden, "VRAM (no CPU access)", 1 << 20, DOMAIN_VRAM, 2, 0)) return 1;
    if (bo_va(&gtt, 0x200000000ull) || bo_va(&ib, 0x200100000ull) || bo_va(&vram, 0x200200000ull) ||
        bo_va(&hidden, 0x200400000ull))
        return 1;

    int fails = 0;
    fails += engine_write(IP_DMA, &ib, &gtt, 16, 0xC0DE0001, "SDMA writes into the GTT buffer") != 0;
    fails += engine_write(IP_DMA, &ib, &vram, 16, 0xC0DE0002, "SDMA writes into the VRAM buffer") != 0;
    fails += engine_write(IP_DMA, &ib, &hidden, 16, 0xC0DE0003, "SDMA writes into hidden VRAM") != 0;
    fails += engine_write(IP_GFX, &ib, &gtt, 32, 0xC0DE0011, "GFX writes into the GTT buffer") != 0;
    fails += engine_write(IP_GFX, &ib, &vram, 32, 0xC0DE0012, "GFX writes into the VRAM buffer") != 0;
    fails += engine_write(IP_COMPUTE, &ib, &gtt, 48, 0xC0DE0021, "compute writes into the GTT buffer") != 0;
    // CPU touches of the buffer kinds radeonsi maps at its start (its first
    // mapped buffer stopped an RX 6700 XT, whose CPU sees 256 MiB of 12 GiB
    // VRAM): ones the driver must first move into the visible window, a
    // cleared one (the fault waits for the clear job), one allowed in VRAM
    // or GTT, and a large one. Each step is on the stick before it runs.
    struct bo t1 = {0}, t2 = {0}, t3 = {0}, t4 = {0};
    fails += bo_create(&t1, "VRAM, no CPU-access flag (moved into the window on the fault)", 65536, DOMAIN_VRAM, 0, 1) != 0;
    fails += bo_create(&t2, "VRAM, cleared (fault waits for the clear job)", 65536, DOMAIN_VRAM, 8, 1) != 0;
    fails += bo_create(&t3, "VRAM or GTT, CPU access, write-combined, explicit sync", 81920, DOMAIN_VRAM | DOMAIN_GTT,
                       0x85, 1) != 0;
    fails += bo_create(&t4, "large VRAM, no flags (16 MiB)", 16u << 20, DOMAIN_VRAM, 0, 1) != 0;
    say(fails ? "work: %d step(s) failed" : "work: all steps passed", fails);
    // Teardown, step by step too: on an RX 6700 XT the machine froze within
    // a second after the work steps (here, or in the next program). Wait
    // first so the kernel log snapshot on the stick catches up.
    sleep(2);
    say("step: unmap the CPU mappings");
    struct bo* all[] = {&gtt, &ib, &vram, &hidden, &t1, &t2, &t3, &t4};
    for (int i = 0; i < 8; i++)
        if (all[i]->cpu)
            munmap(all[i]->cpu, all[i]->size);
    say("step: free the context");
    union ctx f = {.in = {.op = 2, .ctx_id = ctx_id}};
    if (ioctl(fd, AMDGPU_CTX, &f))
        say("note: CTX free: %s", strerror(errno));
    say("step: close the device (frees the buffers and the GPU address space)");
    close(fd);
    say("ok: device closed");
    sleep(2);
    say(fails ? "done: %d step(s) failed" : "done: all steps passed", fails);
    return fails ? 1 : 0;
}
