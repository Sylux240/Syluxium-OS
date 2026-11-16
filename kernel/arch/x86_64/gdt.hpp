#pragma once

#include <lib/types.hpp>

namespace gdt {

// Segment selectors. The user data/code order (data before code) is what
// the SYSRET instruction expects, which we'll use for system calls.
constexpr u16 KERNEL_CODE = 0x08;
constexpr u16 KERNEL_DATA = 0x10;
constexpr u16 USER_DATA   = 0x18 | 3;
constexpr u16 USER_CODE   = 0x20 | 3;
constexpr u16 TSS         = 0x28;

// Interrupt Stack Table slots (1-based, 0 = "don't switch stacks").
// Faults that may happen with a broken stack get their own known-good one.
constexpr u8 IST_DOUBLE_FAULT  = 1;
constexpr u8 IST_NMI           = 2;
constexpr u8 IST_MACHINE_CHECK = 3;
constexpr int IST_COUNT = 3;
constexpr usize IST_STACK_SIZE = 16 * KiB;

struct PACKED Tss {
    u32 reserved0;
    u64 rsp[3];      // stack for ring 0..2 when coming from a lower privilege
    u64 reserved1;
    u64 ist[7];      // Interrupt Stack Table
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
};
static_assert(sizeof(Tss) == 104);

// Every CPU has its own GDT (because each needs its own TSS descriptor).
struct CpuTables {
    u64 entries[7]; // null, kcode, kdata, udata, ucode, TSS (2 slots)
    Tss tss;
};

// Build and load `tables` on the calling CPU. `ist_tops` are the top
// addresses of the IST stacks (IST_COUNT of them).
void load(CpuTables& tables, const u64* ist_tops);

// Load the boot CPU's tables (uses statically allocated IST stacks).
void init_bsp(CpuTables& tables);

// Stack the CPU switches to when an interrupt arrives in user mode.
void set_kernel_stack(CpuTables& tables, u64 rsp0);

} // namespace gdt
