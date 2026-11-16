#include <acpi/acpi.hpp>
#include <arch/x86_64/cpu.hpp>
#include <core/log.hpp>
#include <core/panic.hpp>
#include <mm/vmalloc.hpp>
#include <time/clock.hpp>

namespace clock {

namespace {

enum class Source { None, Hpet, Tsc };

Source g_source = Source::None;

// Conversions avoid division: ns = (ticks * mult) >> 32, with a 128-bit
// intermediate product (one MUL instruction, no runtime library needed).
u64 to_ns(u64 ticks, u64 mult) {
    return static_cast<u64>((static_cast<unsigned __int128>(ticks) * mult) >> 32);
}

// ---- HPET ----
constexpr u32 HPET_CAPS = 0x000;
constexpr u32 HPET_CONFIG = 0x010;
constexpr u32 HPET_COUNTER = 0x0F0;

volatile u64* g_hpet = nullptr;
u64 g_hpet_mult = 0;
u64 g_hpet_start = 0;
bool g_hpet_64bit = false;

u64 hpet_read() { return g_hpet[HPET_COUNTER / 8]; }

bool hpet_init() {
    const auto& info = acpi::hpet();
    if (!info.present)
        return false;
    g_hpet = static_cast<volatile u64*>(vmalloc::map_physical(info.address, 1024));
    if (!g_hpet)
        return false;

    u64 caps = g_hpet[HPET_CAPS / 8];
    u64 period_fs = caps >> 32; // femtoseconds per tick
    if (period_fs == 0 || period_fs > 100'000'000) // spec: at most 100 ns
        return false;
    g_hpet_64bit = caps & (1 << 13);
    g_hpet_mult = (period_fs << 32) / 1'000'000; // fs -> ns, scaled by 2^32

    g_hpet[HPET_CONFIG / 8] = g_hpet[HPET_CONFIG / 8] | 1; // enable main counter
    g_hpet_start = hpet_read();
    return true;
}

u64 hpet_ns() { return to_ns(hpet_read() - g_hpet_start, g_hpet_mult); }

// ---- TSC ----
u64 g_tsc_hz = 0;
u64 g_tsc_mult = 0;
u64 g_tsc_start = 0;

inline u64 rdtsc() {
    u32 lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<u64>(hi) << 32) | lo;
}

bool tsc_invariant() {
    if (arch::cpuid(0x80000000).eax < 0x80000007)
        return false;
    return arch::cpuid(0x80000007).edx & (1u << 8);
}

// Polled PIT channel 2 delay, used only when there is no HPET.
void pit_delay_ms(u32 ms) {
    constexpr u32 PIT_HZ = 1193182;
    u32 count = PIT_HZ * ms / 1000;
    u8 gate = arch::inb(0x61);
    arch::outb(0x61, (gate & ~0x02) | 0x01); // speaker off, gate on
    arch::outb(0x43, 0xB0);                  // channel 2, lo/hi, mode 0
    arch::outb(0x42, count & 0xFF);
    arch::outb(0x42, (count >> 8) & 0xFF);
    while (!(arch::inb(0x61) & 0x20))        // OUT2 goes high when done
        arch::pause();
    arch::outb(0x61, gate);
}

void calibrate_tsc() {
    constexpr u64 WINDOW_NS = 20'000'000;
    u64 t0, t1, ns;
    if (g_hpet) {
        u64 h0 = hpet_ns();
        t0 = rdtsc();
        while (hpet_ns() - h0 < WINDOW_NS)
            arch::pause();
        t1 = rdtsc();
        ns = hpet_ns() - h0;
    } else {
        t0 = rdtsc();
        pit_delay_ms(WINDOW_NS / 1'000'000);
        t1 = rdtsc();
        ns = WINDOW_NS;
    }
    g_tsc_hz = (t1 - t0) * 1'000'000'000 / ns;
    g_tsc_mult = (1'000'000'000ull << 32) / g_tsc_hz;
}

} // namespace

void init() {
    bool have_hpet = hpet_init();
    calibrate_tsc();

    if (tsc_invariant()) {
        g_tsc_start = rdtsc();
        g_source = Source::Tsc;
    } else if (have_hpet && g_hpet_64bit) {
        g_source = Source::Hpet;
    } else {
        // Non-invariant TSC is still monotonic per CPU; best we can do.
        g_tsc_start = rdtsc();
        g_source = Source::Tsc;
        klog::warn("clock: no invariant TSC and no 64-bit HPET, timekeeping may drift");
    }
}

u64 now_ns() {
    switch (g_source) {
    case Source::Tsc:  return to_ns(rdtsc() - g_tsc_start, g_tsc_mult);
    case Source::Hpet: return hpet_ns();
    default:           return 0;
    }
}

void busy_wait_ns(u64 ns) {
    if (g_source == Source::None) {
        // Before init: only HPET may be up (during TSC calibration).
        if (!g_hpet)
            panic("clock: busy_wait_ns() before clock::init()");
        u64 start = hpet_ns();
        while (hpet_ns() - start < ns)
            arch::pause();
        return;
    }
    u64 start = now_ns();
    while (now_ns() - start < ns)
        arch::pause();
}

const char* source_name() {
    switch (g_source) {
    case Source::Tsc:  return tsc_invariant() ? "invariant TSC" : "TSC";
    case Source::Hpet: return "HPET";
    default:           return "none";
    }
}

u64 tsc_frequency_hz() { return g_tsc_hz; }

} // namespace clock
