#include <arch/x86_64/cpu.hpp>
#include <lib/random.hpp>
#include <lib/string.hpp>

namespace random {

namespace {

bool g_rdrand = false;
u64 g_state = 0x9E3779B97F4A7C15ull;

u64 rdtsc() {
    u32 lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<u64>(hi) << 32) | lo;
}

bool rdrand64(u64* out) {
    for (int i = 0; i < 10; i++) {
        bool ok;
        asm volatile("rdrand %0" : "=r"(*out), "=@ccc"(ok));
        if (ok)
            return true;
    }
    return false;
}

} // namespace

void init() {
    g_rdrand = arch::cpuid(1).ecx & (1u << 30);
    g_state ^= rdtsc() * 0xBF58476D1CE4E5B9ull;
}

u64 next_u64() {
    u64 v;
    if (g_rdrand && rdrand64(&v))
        return v;
    // xorshift64*, mixed with the TSC so concurrent callers diverge.
    u64 x = __atomic_load_n(&g_state, __ATOMIC_RELAXED) ^ rdtsc();
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    __atomic_store_n(&g_state, x, __ATOMIC_RELAXED);
    return x * 0x2545F4914F6CDD1Dull;
}

void fill(void* buf, usize len) {
    auto* p = static_cast<u8*>(buf);
    while (len) {
        u64 v = next_u64();
        usize n = min<usize>(len, 8);
        memcpy(p, &v, n);
        p += n;
        len -= n;
    }
}

bool hardware() { return g_rdrand; }

} // namespace random
