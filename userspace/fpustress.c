// Runs a long floating-point / vector computation twice and checks both
// runs agree. Several copies run at once: if the kernel ever mixed up their
// FPU/SSE/AVX registers on a context switch, results would differ.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double work(int seed) {
    double v[256];
    for (int i = 0; i < 256; i++)
        v[i] = (i + 1) * (seed + 0.5);
    double acc = 0;
    for (int round = 0; round < 4000; round++) {
        for (int i = 0; i < 256; i++) {          // vectorizable loop: compiled to SSE/AVX
            v[i] = v[i] * 1.0000001 + (double)(i ^ round) * 1e-9;
            acc += v[i] * 1e-6;
        }
    }
    return acc;
}

int main(int argc, char** argv) {
    int seed = argc > 1 ? atoi(argv[1]) : 1;
    double a = work(seed);
    double b = work(seed);
    if (memcmp(&a, &b, sizeof(a)) != 0) {
        printf("fpustress %d: MISMATCH %.17g vs %.17g\n", seed, a, b);
        return 1;
    }
    return 0;
}
