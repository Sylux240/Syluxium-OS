#pragma once

#include <lib/types.hpp>

namespace arch {

// ---- Port I/O --------------------------------------------------------------

inline void outb(u16 port, u8 value)  { asm volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory"); }
inline void outw(u16 port, u16 value) { asm volatile("outw %0, %1" : : "a"(value), "Nd"(port) : "memory"); }
inline void outl(u16 port, u32 value) { asm volatile("outl %0, %1" : : "a"(value), "Nd"(port) : "memory"); }

inline u8 inb(u16 port) {
    u8 v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
inline u16 inw(u16 port) {
    u16 v;
    asm volatile("inw %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
inline u32 inl(u16 port) {
    u32 v;
    asm volatile("inl %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}

// A write to an unused port takes ~1us, which old devices (PIC) need between commands.
inline void io_wait() { outb(0x80, 0); }

// ---- Interrupt flag / halting ----------------------------------------------

inline void cli()   { asm volatile("cli" ::: "memory"); }
inline void sti()   { asm volatile("sti" ::: "memory"); }
inline void hlt()   { asm volatile("hlt" ::: "memory"); }
inline void pause() { asm volatile("pause" ::: "memory"); }

[[noreturn]] inline void halt_forever() {
    for (;;) {
        cli();
        hlt();
    }
}

inline u64 read_rflags() {
    u64 flags;
    asm volatile("pushfq\n\tpopq %0" : "=r"(flags) : : "memory");
    return flags;
}

constexpr u64 RFLAGS_IF = 1 << 9;

inline bool interrupts_enabled() { return read_rflags() & RFLAGS_IF; }

// ---- Control registers -----------------------------------------------------

#define DEFINE_CR(n)                                                         \
    inline u64 read_cr##n() {                                                \
        u64 v;                                                               \
        asm volatile("mov %%cr" #n ", %0" : "=r"(v));                        \
        return v;                                                            \
    }                                                                        \
    inline void write_cr##n(u64 v) { asm volatile("mov %0, %%cr" #n : : "r"(v) : "memory"); }

DEFINE_CR(0)
DEFINE_CR(2)
DEFINE_CR(3)
DEFINE_CR(4)
#undef DEFINE_CR

// ---- MSRs ------------------------------------------------------------------

inline u64 rdmsr(u32 msr) {
    u32 lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<u64>(hi) << 32) | lo;
}

inline void wrmsr(u32 msr, u64 value) {
    asm volatile("wrmsr" : : "c"(msr), "a"(static_cast<u32>(value)), "d"(static_cast<u32>(value >> 32)));
}

// Like rdmsr/wrmsr, but an MSR the CPU (or hypervisor) doesn't have gives
// false instead of a fatal #GP (interrupts.cpp skips the instruction).
bool rdmsr_safe(u32 msr, u64* value);
bool wrmsr_safe(u32 msr, u64 value);

// ---- CPUID -----------------------------------------------------------------

struct CpuidResult {
    u32 eax, ebx, ecx, edx;
};

inline CpuidResult cpuid(u32 leaf, u32 subleaf = 0) {
    CpuidResult r;
    asm volatile("cpuid" : "=a"(r.eax), "=b"(r.ebx), "=c"(r.ecx), "=d"(r.edx) : "a"(leaf), "c"(subleaf));
    return r;
}

inline void invlpg(uptr addr) { asm volatile("invlpg (%0)" : : "r"(addr) : "memory"); }

} // namespace arch
