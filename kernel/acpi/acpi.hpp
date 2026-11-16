#pragma once

#include <lib/types.hpp>

// ACPI static tables: the firmware's description of the machine (CPUs,
// interrupt controllers, timers, PCIe config space, ...).
//
// This only parses tables. Running AML bytecode (needed for power
// management, shutdown, battery, PCI interrupt routing) comes later by
// porting an AML interpreter (uACPI, MIT licensed).
namespace acpi {

struct PACKED SdtHeader {
    char signature[4];
    u32 length;
    u8 revision;
    u8 checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
};
static_assert(sizeof(SdtHeader) == 36);

// Generic Address Structure.
struct PACKED GenericAddress {
    u8 address_space; // 0 = memory, 1 = I/O port
    u8 bit_width;
    u8 bit_offset;
    u8 access_size;
    u64 address;
};

// ---- Parsed MADT (interrupt controller description) ----

constexpr u32 MAX_IOAPICS = 16;
constexpr u32 MAX_OVERRIDES = 32;
constexpr u32 MAX_LAPIC_NMIS = 16;

struct IoApicInfo {
    u8 id;
    u32 address;
    u32 gsi_base;
};

// ISA IRQ -> global system interrupt remapping (e.g. timer IRQ0 -> GSI 2).
struct InterruptOverride {
    u8 isa_irq;
    u32 gsi;
    bool active_low;
    bool level_triggered;
};

struct LapicNmi {
    u32 processor_uid; // 0xFF / 0xFFFFFFFF = all processors
    u8 lint;           // LINT0 or LINT1
    bool active_low;
    bool level_triggered;
};

struct MadtInfo {
    u64 lapic_address;
    bool has_legacy_pics;
    u32 cpu_count;
    IoApicInfo ioapics[MAX_IOAPICS];
    u32 ioapic_count;
    InterruptOverride overrides[MAX_OVERRIDES];
    u32 override_count;
    LapicNmi nmis[MAX_LAPIC_NMIS];
    u32 nmi_count;
};

struct HpetInfo {
    bool present;
    u64 address;
    u8 number;
    u16 min_tick;
};

void init();

// Find the n-th table with this 4-character signature (e.g. "APIC", "MCFG").
const SdtHeader* find_table(const char* signature, u32 index = 0);

const MadtInfo& madt();
const HpetInfo& hpet();
u8 revision();

} // namespace acpi
