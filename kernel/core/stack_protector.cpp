// The kernel is built with -fstack-protector-strong: functions with arrays
// or address-taken locals put a canary (__stack_chk_guard) below their
// return address and check it before returning, so a stack buffer overflow
// stops the kernel instead of jumping to whatever overwrote the address.
#include <arch/x86_64/cpu.hpp>
#include <core/panic.hpp>
#include <core/stack_protector.hpp>

// Not 0 and with a NUL byte, so string overflows can't reproduce it; made
// random at boot by stack_protector::init().
extern "C" u64 __stack_chk_guard = 0x00e1f00d5ec0ffeeull;

extern "C" [[noreturn]] __attribute__((no_stack_protector)) void __stack_chk_fail() {
    panic("stack buffer overflow detected (a function's canary was overwritten), returning to %p",
          __builtin_return_address(0));
}

namespace stack_protector {

__attribute__((no_stack_protector)) void init() {
    u64 v = 0;
    // RDRAND when the CPU has it (CPUID.1:ECX bit 30), else the time stamp counter.
    if (arch::cpuid(1).ecx & (1u << 30)) {
        unsigned char ok = 0;
        for (int i = 0; i < 10 && !ok; i++)
            asm volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
    }
    u32 lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    v ^= (static_cast<u64>(hi) << 32 | lo) * 0x9E3779B97F4A7C15ull;
    v &= ~0xFFull; // low byte 0: string copies stop there
    __stack_chk_guard = v ? v : 0x00e1f00d5ec0ff00ull;
}

} // namespace stack_protector
