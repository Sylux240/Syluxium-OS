// fputest [seconds] [processes]: user FPU/SSE/AVX registers must survive
// whatever the kernel does meanwhile (context switches, kernel FPU sections
// of GPU drivers). Each process loads known values into xmm0-15 (all 256
// bits of ymm0-15 with AVX) and MXCSR, spins on integer work, reads them
// back and compares, over and over.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int have_avx;

static void load(const uint8_t *p, uint32_t mxcsr) {
    if (have_avx)
        __asm__ volatile(
            "vmovdqu 0(%0), %%ymm0\n vmovdqu 32(%0), %%ymm1\n vmovdqu 64(%0), %%ymm2\n vmovdqu 96(%0), %%ymm3\n"
            "vmovdqu 128(%0), %%ymm4\n vmovdqu 160(%0), %%ymm5\n vmovdqu 192(%0), %%ymm6\n vmovdqu 224(%0), %%ymm7\n"
            "vmovdqu 256(%0), %%ymm8\n vmovdqu 288(%0), %%ymm9\n vmovdqu 320(%0), %%ymm10\n vmovdqu 352(%0), %%ymm11\n"
            "vmovdqu 384(%0), %%ymm12\n vmovdqu 416(%0), %%ymm13\n vmovdqu 448(%0), %%ymm14\n vmovdqu 480(%0), %%ymm15\n"
            : : "r"(p) : "memory");
    else
        __asm__ volatile(
            "movdqu 0(%0), %%xmm0\n movdqu 32(%0), %%xmm1\n movdqu 64(%0), %%xmm2\n movdqu 96(%0), %%xmm3\n"
            "movdqu 128(%0), %%xmm4\n movdqu 160(%0), %%xmm5\n movdqu 192(%0), %%xmm6\n movdqu 224(%0), %%xmm7\n"
            "movdqu 256(%0), %%xmm8\n movdqu 288(%0), %%xmm9\n movdqu 320(%0), %%xmm10\n movdqu 352(%0), %%xmm11\n"
            "movdqu 384(%0), %%xmm12\n movdqu 416(%0), %%xmm13\n movdqu 448(%0), %%xmm14\n movdqu 480(%0), %%xmm15\n"
            : : "r"(p) : "memory");
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
}

static void store(uint8_t *p, uint32_t *mxcsr) {
    if (have_avx)
        __asm__ volatile(
            "vmovdqu %%ymm0, 0(%0)\n vmovdqu %%ymm1, 32(%0)\n vmovdqu %%ymm2, 64(%0)\n vmovdqu %%ymm3, 96(%0)\n"
            "vmovdqu %%ymm4, 128(%0)\n vmovdqu %%ymm5, 160(%0)\n vmovdqu %%ymm6, 192(%0)\n vmovdqu %%ymm7, 224(%0)\n"
            "vmovdqu %%ymm8, 256(%0)\n vmovdqu %%ymm9, 288(%0)\n vmovdqu %%ymm10, 320(%0)\n vmovdqu %%ymm11, 352(%0)\n"
            "vmovdqu %%ymm12, 384(%0)\n vmovdqu %%ymm13, 416(%0)\n vmovdqu %%ymm14, 448(%0)\n vmovdqu %%ymm15, 480(%0)\n"
            : : "r"(p) : "memory");
    else
        __asm__ volatile(
            "movdqu %%xmm0, 0(%0)\n movdqu %%xmm1, 32(%0)\n movdqu %%xmm2, 64(%0)\n movdqu %%xmm3, 96(%0)\n"
            "movdqu %%xmm4, 128(%0)\n movdqu %%xmm5, 160(%0)\n movdqu %%xmm6, 192(%0)\n movdqu %%xmm7, 224(%0)\n"
            "movdqu %%xmm8, 256(%0)\n movdqu %%xmm9, 288(%0)\n movdqu %%xmm10, 320(%0)\n movdqu %%xmm11, 352(%0)\n"
            "movdqu %%xmm12, 384(%0)\n movdqu %%xmm13, 416(%0)\n movdqu %%xmm14, 448(%0)\n movdqu %%xmm15, 480(%0)\n"
            : : "r"(p) : "memory");
    __asm__ volatile("stmxcsr %0" : "=m"(*mxcsr));
}

static int run(int id, int seconds) {
    uint8_t want[512], got[512];
    uint64_t x = 0x9e3779b97f4a7c15ull * (id + 1);
    time_t end = time(NULL) + seconds;
    long rounds = 0;
    int bytes = have_avx ? 32 : 16;
    while (time(NULL) < end) {
        for (int i = 0; i < 512; i++) {
            x = x * 6364136223846793005ull + 1442695040888963407ull;
            want[i] = (uint8_t)(x >> 56);
        }
        uint32_t mx_want = 0x1f80 ^ ((rounds & 3) << 13), mx_got;
        load(want, mx_want);
        volatile uint64_t spin = x;
        for (int i = 0; i < 200000; i++)
            spin = spin * 3 + 1; // integer only: the vector registers just sit there
        store(got, &mx_got);
        for (int r = 0; r < 16; r++) {
            if (memcmp(want + r * 32, got + r * 32, bytes) != 0) {
                printf("fputest %d: register %d corrupted after %ld rounds\n", id, r, rounds);
                return 1;
            }
        }
        if (mx_got != mx_want) {
            printf("fputest %d: MXCSR %#x, expected %#x\n", id, mx_got, mx_want);
            return 1;
        }
        rounds++;
    }
    printf("fputest %d: %ld rounds OK\n", id, rounds);
    return 0;
}

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 30;
    int procs = argc > 2 ? atoi(argv[2]) : (int)sysconf(_SC_NPROCESSORS_ONLN) * 2;
    __builtin_cpu_init();
    have_avx = __builtin_cpu_supports("avx");
    printf("fputest: %d processes, %d s, %s\n", procs, seconds, have_avx ? "AVX (ymm)" : "SSE (xmm)");
    fflush(stdout);
    for (int i = 0; i < procs; i++)
        if (fork() == 0)
            exit(run(i, seconds));
    int failed = 0, status;
    while (wait(&status) > 0)
        if (!WIFEXITED(status) || WEXITSTATUS(status))
            failed++;
    printf("fputest: %s (%d of %d processes failed)\n", failed ? "FAILED" : "all passed", failed, procs);
    return failed != 0;
}
