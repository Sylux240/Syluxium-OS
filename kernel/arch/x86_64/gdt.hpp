#pragma once

#include <lib/types.hpp>

namespace gdt {

// Segment selectors: the same values as Linux, because user programs depend
// on them. Wine's WoW64 mode (32-bit Windows programs in a 64-bit process)
// far-jumps to the hard-coded 32-bit user code selector 0x23, and the
// 32-bit/64-bit code and data order is also what SYSRET expects
// (STAR[63:48] = 0x20: 32-bit CS 0x23, SS 0x2b, 64-bit CS 0x33).
constexpr u16 KERNEL_CODE = 0x08;
constexpr u16 KERNEL_DATA = 0x10;
// ia32 (compatibility mode) user code: L=0, D=1. 32-bit programs run with
// this CS; their data/stack segment is the flat USER_DATA below.
constexpr u16 USER32_CODE = 0x20 | 3;
constexpr u16 USER_DATA   = 0x28 | 3;
constexpr u16 USER_CODE   = 0x30 | 3;
constexpr u16 TSS         = 0x38;   // 16-byte descriptor: entries 7 and 8
// Thread-local storage descriptors (set_thread_area; entries 12-14 =
// selectors 0x63/0x6b/0x73, as on Linux). Each thread owns TLS_ENTRIES
// descriptors that the scheduler copies into this CPU's GDT slots when the
// thread is switched in, like Linux does.
constexpr int TLS_FIRST_ENTRY = 12;
constexpr int TLS_ENTRIES = 3;
constexpr u16 tls_selector(int i) { return static_cast<u16>(((TLS_FIRST_ENTRY + i) << 3) | 3); }

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
    // null, kcode, kdata, -, ucode32, udata, ucode, TSS (2 slots), -, -, -, TLS x3
    u64 entries[TLS_FIRST_ENTRY + TLS_ENTRIES];
    Tss tss;
};

// Build and load `tables` on the calling CPU. `ist_tops` are the top
// addresses of the IST stacks (IST_COUNT of them).
void load(CpuTables& tables, const u64* ist_tops);

// Load the boot CPU's tables (uses statically allocated IST stacks).
void init_bsp(CpuTables& tables);

// Copy a thread's TLS descriptors into this CPU's GDT.
void install_tls(CpuTables& tables, const u64* descs);

// Stack the CPU switches to when an interrupt arrives in user mode.
void set_kernel_stack(CpuTables& tables, u64 rsp0);

} // namespace gdt
