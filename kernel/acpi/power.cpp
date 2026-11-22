// Power off and restart through ACPI (and the classic fallbacks).
//
// Power off: the FADT gives the PM1a/PM1b control registers; the sleep type
// values for S5 ("soft off") live in the DSDT as the AML package \_S5_. A
// full AML interpreter isn't needed for that: the package is a constant, so
// it's found by its byte pattern (as most hobby and many small kernels do).
//
// Restart: the FADT reset register, then the 8042 keyboard controller's
// reset line, then the PCI reset control port 0xCF9, then a triple fault.

#include <acpi/devices.hpp>
#include <acpi/acpi.hpp>
#include <acpi/power.hpp>
#include <arch/x86_64/cpu.hpp>
#include <core/bootlog.hpp>
#include <core/log.hpp>
#include <lib/string.hpp>
#include <mm/layout.hpp>

namespace acpi {

namespace {

struct Fadt {
    bool valid = false;
    u64 dsdt = 0;
    u32 smi_cmd = 0;
    u8 acpi_enable = 0;
    u64 pm1a_cnt = 0, pm1b_cnt = 0; // I/O ports
    u32 flags = 0;
    u16 boot_arch = 0;
    GenericAddress reset_reg{};
    u8 reset_value = 0;
} g_fadt;

struct SleepType {
    bool found = false;
    u8 a = 0, b = 0;
} g_s5, g_s3;

template <typename T>
T field(const u8* t, u32 len, u32 off) {
    T v{};
    if (off + sizeof(T) <= len)
        memcpy(&v, t + off, sizeof(T));
    return v;
}

// AML: NameOp(0x08) "_Sx_" PackageOp(0x12) PkgLength NumElements <elements>,
// each element ZeroOp(0x00) / OneOp(0x01) / BytePrefix(0x0A) <byte> / ...
bool parse_sleep_package(const u8* p, const u8* end, SleepType* out) {
    if (p + 5 > end || p[4] != 0x12)
        return false;
    p += 5;
    u8 lead = *p;
    p += 1 + (lead >> 6); // PkgLength: bits 7-6 = number of extra length bytes
    if (p >= end)
        return false;
    p++; // NumElements
    u8 values[2] = {};
    for (int i = 0; i < 2 && p < end; i++) {
        if (*p == 0x0A && p + 1 < end) { // BytePrefix
            values[i] = p[1];
            p += 2;
        } else if (*p == 0x00 || *p == 0x01) { // ZeroOp / OneOp
            values[i] = *p;
            p++;
        } else if (*p == 0xFF) { // OnesOp
            values[i] = 0xFF;
            p++;
        } else {
            return false;
        }
    }
    out->found = true;
    out->a = values[0] & 7;
    out->b = values[1] & 7;
    return true;
}

void find_sleep_type(const SdtHeader* table, const char* name, SleepType* out) {
    if (!table || out->found)
        return;
    auto* bytes = reinterpret_cast<const u8*>(table);
    const u8* end = bytes + table->length;
    for (const u8* p = bytes + sizeof(SdtHeader); p + 4 < end; p++) {
        if (memcmp(p, name, 4) != 0)
            continue;
        // Preceded by NameOp (possibly with a root/parent prefix '\\' or '^').
        bool named = (p > bytes && p[-1] == 0x08) || (p > bytes + 1 && p[-2] == 0x08 && (p[-1] == '\\' || p[-1] == '^'));
        if (named && parse_sleep_package(p, end, out))
            return;
    }
}

void write_pm1(u16 value_a, u16 value_b) {
    if (g_fadt.pm1a_cnt)
        arch::outw(static_cast<u16>(g_fadt.pm1a_cnt), value_a);
    if (g_fadt.pm1b_cnt)
        arch::outw(static_cast<u16>(g_fadt.pm1b_cnt), value_b);
}

} // namespace

void power_init() {
    const SdtHeader* h = find_table("FACP");
    if (!h)
        return;
    auto* t = reinterpret_cast<const u8*>(h);
    u32 len = h->length;
    g_fadt.valid = true;
    g_fadt.dsdt = field<u32>(t, len, 40);
    u64 x_dsdt = field<u64>(t, len, 140);
    if (x_dsdt)
        g_fadt.dsdt = x_dsdt;
    g_fadt.smi_cmd = field<u32>(t, len, 48);
    g_fadt.acpi_enable = field<u8>(t, len, 52);
    g_fadt.pm1a_cnt = field<u32>(t, len, 64);
    g_fadt.pm1b_cnt = field<u32>(t, len, 68);
    g_fadt.boot_arch = field<u16>(t, len, 109);
    g_fadt.flags = field<u32>(t, len, 112);
    g_fadt.reset_reg = field<GenericAddress>(t, len, 116);
    g_fadt.reset_value = field<u8>(t, len, 128);
    // ACPI 2.0+ extended addresses (only I/O-space PM1 blocks are used).
    GenericAddress xa = field<GenericAddress>(t, len, 172), xb = field<GenericAddress>(t, len, 184);
    if (xa.address && xa.address_space == 1)
        g_fadt.pm1a_cnt = xa.address;
    if (xb.address && xb.address_space == 1)
        g_fadt.pm1b_cnt = xb.address;

    if (g_fadt.dsdt)
        find_sleep_type(mm::phys_to_virt<const SdtHeader>(g_fadt.dsdt), "_S5_", &g_s5);
    for (u32 i = 0; !g_s5.found && find_table("SSDT", i); i++)
        find_sleep_type(find_table("SSDT", i), "_S5_", &g_s5);
    if (g_fadt.dsdt)
        find_sleep_type(mm::phys_to_virt<const SdtHeader>(g_fadt.dsdt), "_S3_", &g_s3);

    klog::ok("ACPI power: PM1a_CNT %#lx%s, S5 %s (%u/%u), S3 %s, reset register %s", g_fadt.pm1a_cnt,
             g_fadt.pm1b_cnt ? " + PM1b" : "", g_s5.found ? "found" : "missing", g_s5.a, g_s5.b,
             g_s3.found ? "found" : "missing", (g_fadt.flags & (1u << 10)) ? "yes" : "no");
}

bool has_8042() {
    // IA-PC boot architecture flags, bit 1: "8042 present". Many laptops get
    // this wrong (clear although they have one), so callers only use it as
    // a hint; revision 1 FADTs don't have the field at all.
    return !g_fadt.valid || revision() < 2 || (g_fadt.boot_arch & 0x2);
}

bool can_sleep() { return g_s3.found && g_fadt.pm1a_cnt; }
SleepState s3_state() { return SleepState{g_s3.a, g_s3.b}; }
u64 pm1a_control() { return g_fadt.pm1a_cnt; }
u64 pm1b_control() { return g_fadt.pm1b_cnt; }

void enable_acpi_mode() {
    if (!g_fadt.pm1a_cnt)
        return;
    if (arch::inw(static_cast<u16>(g_fadt.pm1a_cnt)) & 1) // SCI_EN: already in ACPI mode
        return;
    if (g_fadt.smi_cmd && g_fadt.acpi_enable) {
        arch::outb(static_cast<u16>(g_fadt.smi_cmd), g_fadt.acpi_enable);
        for (int i = 0; i < 300 && !(arch::inw(static_cast<u16>(g_fadt.pm1a_cnt)) & 1); i++)
            for (int k = 0; k < 10000; k++)
                arch::io_wait();
    }
}

[[noreturn]] void power_off() {
    // With the AML interpreter: the firmware's full sequence (_PTS, wake
    // events disabled, then S5) - without it some machines wake right up
    // again, which looks like a reboot. Falls through if it fails.
    if (interpreter_ready()) {
        klog::info("Power off: ACPI S5 through the firmware's methods (_PTS, _S5)");
        bootlog::save();
        enter_s5();
        klog::warn("Power off: the firmware's S5 sequence returned; writing the sleep registers directly");
        bootlog::save();
    }
    arch::cli();
    if (g_fadt.pm1a_cnt && g_s5.found) {
        klog::info("Power off: PM1a 0x%llx PM1b 0x%llx, S5 sleep type %u/%u", static_cast<unsigned long long>(g_fadt.pm1a_cnt),
                   static_cast<unsigned long long>(g_fadt.pm1b_cnt), g_s5.a, g_s5.b);
        bootlog::save();
        enable_acpi_mode();
        constexpr u16 SLP_EN = 1u << 13;
        u16 a = arch::inw(static_cast<u16>(g_fadt.pm1a_cnt)) & ~(7u << 10 | SLP_EN);
        u16 b = g_fadt.pm1b_cnt ? (arch::inw(static_cast<u16>(g_fadt.pm1b_cnt)) & ~(7u << 10 | SLP_EN)) : 0;
        // As Linux does: the sleep type first, then the same with SLP_EN
        // (some firmware, Apple's among it, only acts on that order).
        write_pm1(static_cast<u16>(a | (g_s5.a << 10)), static_cast<u16>(b | (g_s5.b << 10)));
        write_pm1(static_cast<u16>(a | (g_s5.a << 10) | SLP_EN), static_cast<u16>(b | (g_s5.b << 10) | SLP_EN));
        for (int k = 0; k < 1000000; k++)
            arch::io_wait();
    }
    // Emulators without (or with unparsed) ACPI tables.
    arch::outw(0x604, 0x2000);  // QEMU q35/i440fx (PIIX4 PM)
    arch::outw(0xB004, 0x2000); // Bochs, old QEMU
    arch::outw(0x4004, 0x3400); // VirtualBox
    klog::warn("Power off failed: it's now safe to turn off the computer.");
    for (;;)
        arch::hlt();
}

[[noreturn]] void restart() {
    arch::cli();
    // 1) ACPI reset register (RESET_REG_SUP).
    if ((g_fadt.flags & (1u << 10)) && g_fadt.reset_reg.address) {
        const GenericAddress& r = g_fadt.reset_reg;
        if (r.address_space == 1) {
            arch::outb(static_cast<u16>(r.address), g_fadt.reset_value);
        } else if (r.address_space == 0) {
            *mm::phys_to_virt<volatile u8>(r.address) = g_fadt.reset_value;
        }
        for (int k = 0; k < 100000; k++)
            arch::io_wait();
    }
    // 2) Keyboard controller: pulse the CPU reset line.
    for (int i = 0; i < 100000 && (arch::inb(0x64) & 0x02); i++)
        arch::io_wait();
    arch::outb(0x64, 0xFE);
    for (int k = 0; k < 100000; k++)
        arch::io_wait();
    // 3) PCI reset control register: hard reset.
    arch::outb(0xCF9, 0x02);
    arch::io_wait();
    arch::outb(0xCF9, 0x06);
    for (int k = 0; k < 100000; k++)
        arch::io_wait();
    // 4) Triple fault: an empty IDT and an exception.
    struct PACKED {
        u16 limit;
        u64 base;
    } empty{0, 0};
    asm volatile("lidt %0; int3" : : "m"(empty));
    for (;;)
        arch::hlt();
}

} // namespace acpi
