#pragma once

#include <lib/types.hpp>

namespace interrupts {

// Register state saved by isr_common (isr.S). Field order must match the
// push order there: the last register pushed is at the lowest address.
struct Frame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vector;
    u64 error_code; // pushed by the CPU for some exceptions, else 0 by the stub
    // Pushed by the CPU on every interrupt:
    u64 rip, cs, rflags, rsp, ss;
};

using Handler = void (*)(Frame* frame);

// Vector numbers.
constexpr u8 VEC_DIVIDE_ERROR      = 0;
constexpr u8 VEC_DEBUG             = 1;
constexpr u8 VEC_NMI               = 2;
constexpr u8 VEC_BREAKPOINT        = 3;
constexpr u8 VEC_INVALID_OPCODE    = 6;
constexpr u8 VEC_DOUBLE_FAULT      = 8;
constexpr u8 VEC_GENERAL_PROTECTION = 13;
constexpr u8 VEC_PAGE_FAULT        = 14;
constexpr u8 VEC_MACHINE_CHECK     = 18;
constexpr u8 VEC_LEGACY_PIC_BASE    = 0x20; // 0x20-0x2F: remapped (masked) 8259 PIC
constexpr u8 VEC_DEVICE_FIRST      = 0x30; // 0x30-0xEF: allocated to devices
constexpr u8 VEC_DEVICE_LAST       = 0xEF;
constexpr u8 VEC_TIMER             = 0xF0; // local APIC timer (scheduler tick)
constexpr u8 VEC_RESCHEDULE        = 0xF1; // IPI: "check your run queue"
constexpr u8 VEC_TLB_SHOOTDOWN     = 0xF2; // IPI: flush TLB entries
constexpr u8 VEC_PANIC_HALT        = 0xF3; // IPI: another CPU panicked, stop
constexpr u8 VEC_APIC_ERROR        = 0xFE;
constexpr u8 VEC_SPURIOUS          = 0xFF;

void init();      // build the IDT and load it on the boot CPU
void load_idt();  // load the (shared) IDT on another CPU
void set_handler(u8 vector, Handler handler);

// Reserve a free device vector; 0 if none are left.
u8 allocate_vector();
void dump_frame(const Frame* frame);

// Print the exception and panic.
[[noreturn]] void fatal_exception(Frame* frame);
const char* exception_name(u64 vector);

// Called for CPU exceptions raised in user mode that no handler resolved
// (the process layer installs one that kills the offending process).
using UserFaultHook = void (*)(Frame* frame);
void set_user_fault_hook(UserFaultHook hook);

// Called (interrupts off) whenever an interrupt is about to return to user
// mode; the process layer delivers pending signals from here.
void set_return_to_user_hook(UserFaultHook hook);

} // namespace interrupts
