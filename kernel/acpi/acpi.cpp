#include <acpi/acpi.hpp>
#include <boot/limine_requests.hpp>
#include <core/log.hpp>
#include <core/panic.hpp>
#include <lib/string.hpp>
#include <mm/layout.hpp>

namespace acpi {

namespace {

struct PACKED Rsdp {
    char signature[8]; // "RSD PTR "
    u8 checksum;
    char oem_id[6];
    u8 revision;       // 0 = ACPI 1.0 (RSDT only), 2+ = has XSDT
    u32 rsdt_address;
    // ACPI 2.0+
    u32 length;
    u64 xsdt_address;
    u8 extended_checksum;
    u8 reserved[3];
};

struct PACKED Madt {
    SdtHeader header;
    u32 lapic_address;
    u32 flags; // bit 0: legacy 8259 PICs present
};

struct PACKED MadtEntry {
    u8 type;
    u8 length;
};

struct PACKED Hpet {
    SdtHeader header;
    u32 event_timer_block_id;
    GenericAddress address;
    u8 hpet_number;
    u16 min_tick;
    u8 page_protection;
};

constexpr u32 MAX_TABLES = 64;

const SdtHeader* g_tables[MAX_TABLES];
u32 g_table_count = 0;
u8 g_revision = 0;
MadtInfo g_madt;
HpetInfo g_hpet;

bool checksum_ok(const void* data, usize len) {
    u8 sum = 0;
    auto bytes = static_cast<const u8*>(data);
    for (usize i = 0; i < len; i++)
        sum += bytes[i];
    return sum == 0;
}

const SdtHeader* map_table(u64 phys) {
    // Limine guarantees ACPI tables live in regions our direct map covers.
    return mm::phys_to_virt<const SdtHeader>(phys);
}

void add_table(u64 phys) {
    if (!phys || g_table_count >= MAX_TABLES)
        return;
    const SdtHeader* h = map_table(phys);
    if (!checksum_ok(h, h->length)) {
        klog::warn("ACPI: table %.4s at %#lx has a bad checksum, ignoring", h->signature, phys);
        return;
    }
    g_tables[g_table_count++] = h;
}

void parse_madt() {
    auto* madt = reinterpret_cast<const Madt*>(find_table("APIC"));
    if (!madt)
        panic("ACPI: no MADT; this machine has no APIC description");

    g_madt.lapic_address = madt->lapic_address;
    g_madt.has_legacy_pics = madt->flags & 1;

    auto* p = reinterpret_cast<const u8*>(madt) + sizeof(Madt);
    auto* end = reinterpret_cast<const u8*>(madt) + madt->header.length;
    while (p + sizeof(MadtEntry) <= end) {
        auto* e = reinterpret_cast<const MadtEntry*>(p);
        if (e->length < 2)
            break; // malformed; stop rather than loop forever

        switch (e->type) {
        case 0: { // Processor Local APIC
            u32 flags;
            memcpy(&flags, p + 4, 4);
            if (flags & 3) // enabled or online-capable
                g_madt.cpu_count++;
            break;
        }
        case 1: // I/O APIC
            if (g_madt.ioapic_count < MAX_IOAPICS) {
                IoApicInfo& io = g_madt.ioapics[g_madt.ioapic_count++];
                io.id = p[2];
                memcpy(&io.address, p + 4, 4);
                memcpy(&io.gsi_base, p + 8, 4);
            }
            break;
        case 2: // Interrupt Source Override
            if (g_madt.override_count < MAX_OVERRIDES) {
                InterruptOverride& o = g_madt.overrides[g_madt.override_count++];
                u16 flags;
                o.isa_irq = p[3];
                memcpy(&o.gsi, p + 4, 4);
                memcpy(&flags, p + 8, 2);
                o.active_low = (flags & 3) == 3;
                o.level_triggered = ((flags >> 2) & 3) == 3;
            }
            break;
        case 4: // Local APIC NMI
            if (g_madt.nmi_count < MAX_LAPIC_NMIS) {
                LapicNmi& n = g_madt.nmis[g_madt.nmi_count++];
                u16 flags;
                n.processor_uid = p[2] == 0xFF ? 0xFFFFFFFF : p[2];
                memcpy(&flags, p + 3, 2);
                n.lint = p[5];
                n.active_low = (flags & 3) == 3;
                n.level_triggered = ((flags >> 2) & 3) == 3;
            }
            break;
        case 5: // Local APIC address override (64-bit)
            memcpy(&g_madt.lapic_address, p + 4, 8);
            break;
        case 9: { // Processor Local x2APIC
            u32 flags;
            memcpy(&flags, p + 8, 4);
            if (flags & 3)
                g_madt.cpu_count++;
            break;
        }
        case 0xA: // Local x2APIC NMI
            if (g_madt.nmi_count < MAX_LAPIC_NMIS) {
                LapicNmi& n = g_madt.nmis[g_madt.nmi_count++];
                u16 flags;
                memcpy(&flags, p + 2, 2);
                memcpy(&n.processor_uid, p + 4, 4);
                n.lint = p[8];
                n.active_low = (flags & 3) == 3;
                n.level_triggered = ((flags >> 2) & 3) == 3;
            }
            break;
        default:
            break;
        }
        p += e->length;
    }
}

void parse_hpet() {
    auto* hpet = reinterpret_cast<const Hpet*>(find_table("HPET"));
    if (!hpet || hpet->address.address_space != 0)
        return;
    g_hpet.present = true;
    g_hpet.address = hpet->address.address;
    g_hpet.number = hpet->hpet_number;
    g_hpet.min_tick = hpet->min_tick;
}

} // namespace

void init() {
    auto* rsdp = static_cast<const Rsdp*>(boot::rsdp());
    if (!rsdp)
        panic("ACPI: bootloader found no RSDP (is this a non-ACPI machine?)");
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0 || !checksum_ok(rsdp, 20))
        panic("ACPI: RSDP is corrupt");

    g_revision = rsdp->revision;
    if (rsdp->revision >= 2 && rsdp->xsdt_address) {
        const SdtHeader* xsdt = map_table(rsdp->xsdt_address);
        usize n = (xsdt->length - sizeof(SdtHeader)) / 8;
        auto* entries = reinterpret_cast<const u8*>(xsdt) + sizeof(SdtHeader);
        for (usize i = 0; i < n; i++) {
            u64 phys;
            memcpy(&phys, entries + i * 8, 8); // entries are not 8-byte aligned
            add_table(phys);
        }
    } else {
        const SdtHeader* rsdt = map_table(rsdp->rsdt_address);
        usize n = (rsdt->length - sizeof(SdtHeader)) / 4;
        auto* entries = reinterpret_cast<const u8*>(rsdt) + sizeof(SdtHeader);
        for (usize i = 0; i < n; i++) {
            u32 phys;
            memcpy(&phys, entries + i * 4, 4);
            add_table(phys);
        }
    }

    // List tables on serial; it's handy when bringing up real hardware.
    kprintf_serial("    ACPI tables:");
    for (u32 i = 0; i < g_table_count; i++)
        kprintf_serial(" %.4s", g_tables[i]->signature);
    kprintf_serial("\n");

    parse_madt();
    parse_hpet();
}

const SdtHeader* find_table(const char* signature, u32 index) {
    for (u32 i = 0; i < g_table_count; i++) {
        if (memcmp(g_tables[i]->signature, signature, 4) == 0 && index-- == 0)
            return g_tables[i];
    }
    return nullptr;
}

const MadtInfo& madt() { return g_madt; }
const HpetInfo& hpet() { return g_hpet; }
u8 revision() { return g_revision; }

} // namespace acpi
