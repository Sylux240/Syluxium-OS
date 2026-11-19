#include <arch/x86_64/gdt.hpp>
#include <lib/string.hpp>

// In 64-bit mode segmentation is mostly gone, but the CPU still needs a GDT
// for the privilege level (ring 0 vs ring 3) and a TSS for the stacks used
// on interrupts.

namespace gdt {

namespace {

struct PACKED Gdtr {
    u16 limit;
    u64 base;
};

ALIGNED(16) u8 g_bsp_ist_stacks[IST_COUNT][IST_STACK_SIZE];

// Access byte: P=present, DPL, S=code/data, E=executable, RW
// Flags nibble: L=64-bit code, G=4K granularity, DB (must be 0 for L=1)
constexpr u64 KERNEL_CODE_DESC = 0x00AF9A000000FFFF; // P DPL0 S E R, L G
constexpr u64 KERNEL_DATA_DESC = 0x00CF92000000FFFF; // P DPL0 S W,   DB G
constexpr u64 USER_DATA_DESC   = 0x00CFF2000000FFFF; // P DPL3 S W,   DB G
constexpr u64 USER_CODE_DESC   = 0x00AFFA000000FFFF; // P DPL3 S E R, L G
constexpr u64 USER32_CODE_DESC = 0x00CFFA000000FFFF; // P DPL3 S E R, DB G (compatibility mode)

} // namespace

void load(CpuTables& t, const u64* ist_tops) {
    memset(&t.tss, 0, sizeof(t.tss));
    t.tss.iomap_base = sizeof(Tss); // no I/O permission bitmap
    for (int i = 0; i < IST_COUNT; i++)
        t.tss.ist[i] = ist_tops[i];

    u64 base = reinterpret_cast<u64>(&t.tss);
    u64 limit = sizeof(Tss) - 1;

    memset(t.entries, 0, sizeof(t.entries));
    t.entries[KERNEL_CODE >> 3] = KERNEL_CODE_DESC;
    t.entries[KERNEL_DATA >> 3] = KERNEL_DATA_DESC;
    t.entries[USER32_CODE >> 3] = USER32_CODE_DESC;
    t.entries[USER_DATA >> 3] = USER_DATA_DESC;
    t.entries[USER_CODE >> 3] = USER_CODE_DESC;
    // 64-bit TSS descriptor is 16 bytes. Type 0x89 = present, available TSS.
    t.entries[TSS >> 3] = (limit & 0xFFFF)
                        | ((base & 0xFFFFFF) << 16)
                        | (0x89ULL << 40)
                        | (((limit >> 16) & 0xF) << 48)
                        | (((base >> 24) & 0xFF) << 56);
    t.entries[(TSS >> 3) + 1] = base >> 32;
    for (int i = 0; i < TLS_ENTRIES; i++)
        t.entries[TLS_FIRST_ENTRY + i] = 0; // not present until a 32-bit thread installs its own

    Gdtr gdtr{sizeof(t.entries) - 1, reinterpret_cast<u64>(t.entries)};

    // Load the GDT, then reload CS with a far return (you can't `mov` into CS)
    // and the data segment registers with plain moves. FS/GS are left alone:
    // their bases are set through MSRs (GS holds the per-CPU pointer).
    asm volatile(
        "lgdt %[gdtr]\n\t"
        "pushq %[cs]\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "movw %[ds], %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%ss\n\t"
        "ltr %[tss]"
        :
        : [gdtr] "m"(gdtr), [cs] "i"(KERNEL_CODE), [ds] "i"(KERNEL_DATA), [tss] "r"(TSS)
        : "rax", "memory");
}

void init_bsp(CpuTables& tables) {
    u64 tops[IST_COUNT];
    for (int i = 0; i < IST_COUNT; i++)
        tops[i] = reinterpret_cast<u64>(&g_bsp_ist_stacks[i][IST_STACK_SIZE]); // stacks grow down
    load(tables, tops);
}

void install_tls(CpuTables& tables, const u64* descs) {
    for (int i = 0; i < TLS_ENTRIES; i++)
        tables.entries[TLS_FIRST_ENTRY + i] = descs[i];
}

void set_kernel_stack(CpuTables& tables, u64 rsp0) { tables.tss.rsp[0] = rsp0; }

} // namespace gdt
