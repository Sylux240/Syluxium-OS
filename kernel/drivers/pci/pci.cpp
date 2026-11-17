#include <acpi/acpi.hpp>
#include <arch/x86_64/percpu.hpp>
#include <core/log.hpp>
#include <drivers/pci/pci.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/vmalloc.hpp>

namespace pci {

namespace {

struct PACKED McfgEntry {
    u64 base;
    u16 segment;
    u8 start_bus;
    u8 end_bus;
    u32 reserved;
};

struct EcamRegion {
    u64 base;
    u16 segment;
    u8 start_bus, end_bus;
    volatile u8* mapped; // only buses that exist get mapped, lazily
};

constexpr u32 MAX_REGIONS = 8;
constexpr u32 MAX_FUNCTIONS = 256;

EcamRegion g_regions[MAX_REGIONS];
u32 g_region_count = 0;
Function g_functions[MAX_FUNCTIONS];
u32 g_count = 0;

// Each bus is 1 MiB of ECAM (32 devices x 8 functions x 4 KiB). Map per bus on demand.
volatile u8* g_bus_maps[MAX_REGIONS][256];

volatile u8* config_window(u32 region, u8 bus, u8 device, u8 function) {
    EcamRegion& r = g_regions[region];
    volatile u8*& m = g_bus_maps[region][bus];
    if (!m) {
        u64 phys = r.base + (static_cast<u64>(bus - r.start_bus) << 20);
        m = static_cast<volatile u8*>(vmalloc::map_physical(phys, 1 * MiB));
        if (!m)
            return nullptr;
    }
    return m + ((static_cast<u32>(device) << 15) | (static_cast<u32>(function) << 12));
}

u32 raw_read32(volatile u8* cfg, u16 off) { return *reinterpret_cast<volatile u32*>(cfg + off); }
void raw_write32(volatile u8* cfg, u16 off, u32 v) { *reinterpret_cast<volatile u32*>(cfg + off) = v; }

dev::Category category_for(u8 cls, u8 sub) {
    switch (cls) {
    case 0x01: return dev::Category::Storage;
    case 0x02: return dev::Category::Network;
    case 0x03: return dev::Category::Display;
    case 0x04: return sub == 0x03 ? dev::Category::Audio : dev::Category::Multimedia;
    case 0x06: return dev::Category::Bridge;
    case 0x07: return dev::Category::Serial;
    case 0x09: return dev::Category::Input;
    case 0x0C: return sub == 0x03 ? dev::Category::Usb : dev::Category::System;
    case 0x0D: return dev::Category::Network; // wireless
    default:   return dev::Category::System;
    }
}

void size_bars(Function& f) {
    u8 type = f.header_type & 0x7F;
    int count = type == 0 ? 6 : (type == 1 ? 2 : 0);
    // Stop decoding while BARs hold all-ones during sizing.
    u16 cmd = read16(f, 0x04);
    write16(f, 0x04, cmd & ~0x3);
    for (int i = 0; i < count; i++) {
        u16 off = 0x10 + i * 4;
        u32 orig = read32(f, off);
        write32(f, off, 0xFFFFFFFF);
        u32 mask = read32(f, off);
        write32(f, off, orig);
        if (mask == 0 || (mask == 0xFFFFFFFF && orig == 0))
            continue;
        Bar& b = f.bars[i];
        if (orig & 1) { // I/O space
            b.is_io = true;
            b.address = orig & ~0x3u;
            b.size = (~(mask & ~0x3u) + 1) & 0xFFFF;
        } else {
            b.prefetchable = orig & 0x8;
            u64 addr = orig & ~0xFull;
            u64 size_mask = mask & ~0xFull;
            if (((orig >> 1) & 3) == 2 && i + 1 < count) { // 64-bit BAR: uses two slots
                b.is_64bit = true;
                u32 orig_hi = read32(f, off + 4);
                write32(f, off + 4, 0xFFFFFFFF);
                u32 mask_hi = read32(f, off + 4);
                write32(f, off + 4, orig_hi);
                addr |= static_cast<u64>(orig_hi) << 32;
                size_mask |= static_cast<u64>(mask_hi) << 32;
                b.size = ~size_mask + 1;
                i++;
            } else {
                b.size = static_cast<u32>(~static_cast<u32>(size_mask) + 1);
            }
            b.address = addr;
        }
    }
    write16(f, 0x04, cmd);
}

void find_capabilities(Function& f) {
    if (!(read16(f, 0x06) & 0x10)) // status: capabilities list present
        return;
    u8 ptr = read8(f, 0x34) & 0xFC;
    for (int guard = 0; ptr && guard < 48; guard++) {
        u8 id = read8(f, ptr);
        if (id == 0x05) f.msi_cap = ptr;
        else if (id == 0x11) f.msix_cap = ptr;
        else if (id == 0x10) f.pcie_cap = ptr;
        ptr = read8(f, ptr + 1) & 0xFC;
    }
}

void register_function(Function& f) {
    auto* d = new dev::Device;
    d->bus = dev::Bus::Pci;
    snprintf(d->bus_id, sizeof(d->bus_id), "%04x:%02x:%02x.%x", f.segment, f.bus, f.device, f.function);
    snprintf(d->name, sizeof(d->name), "%s", class_name(f.class_code, f.subclass, f.prog_if));
    d->category = category_for(f.class_code, f.subclass);
    d->vendor = f.vendor_id;
    d->device = f.device_id;
    d->class_code = (static_cast<u32>(f.class_code) << 16) | (static_cast<u32>(f.subclass) << 8) | f.prog_if;
    d->revision = f.revision;
    if ((f.header_type & 0x7F) == 0) {
        d->subsystem_vendor = read16(f, 0x2C);
        d->subsystem_device = read16(f, 0x2E);
    }
    for (const Bar& b : f.bars)
        if (b.size)
            d->add_resource(b.is_io ? dev::Resource::Io : dev::Resource::Mmio, b.address, b.size);
    if (f.irq_pin)
        d->add_resource(dev::Resource::Irq, f.irq_line, 1);
    d->bus_data = &f;
    f.entry = d;
    dev::add(d);
}

void scan_function(u32 region, u8 bus, u8 device, u8 function) {
    volatile u8* cfg = config_window(region, bus, device, function);
    if (!cfg || g_count >= MAX_FUNCTIONS)
        return;
    u32 id = raw_read32(cfg, 0);
    if ((id & 0xFFFF) == 0xFFFF)
        return;
    Function& f = g_functions[g_count++];
    f = Function{};
    f.config = cfg;
    f.segment = g_regions[region].segment;
    f.bus = bus;
    f.device = device;
    f.function = function;
    f.vendor_id = id & 0xFFFF;
    f.device_id = id >> 16;
    u32 classreg = raw_read32(cfg, 0x08);
    f.revision = classreg & 0xFF;
    f.prog_if = (classreg >> 8) & 0xFF;
    f.subclass = (classreg >> 16) & 0xFF;
    f.class_code = classreg >> 24;
    f.header_type = read8(f, 0x0E);
    f.irq_line = read8(f, 0x3C);
    f.irq_pin = read8(f, 0x3D);
    size_bars(f);
    find_capabilities(f);
    register_function(f);
}

} // namespace

u8 read8(const Function& f, u16 off) { return f.config[off]; }
u16 read16(const Function& f, u16 off) { return *reinterpret_cast<volatile u16*>(f.config + off); }
u32 read32(const Function& f, u16 off) { return raw_read32(f.config, off); }
void write8(const Function& f, u16 off, u8 v) { f.config[off] = v; }
void write16(const Function& f, u16 off, u16 v) { *reinterpret_cast<volatile u16*>(f.config + off) = v; }
void write32(const Function& f, u16 off, u32 v) { raw_write32(f.config, off, v); }

void init() {
    auto* mcfg = acpi::find_table("MCFG");
    if (!mcfg) {
        klog::warn("PCI: no MCFG table (legacy-only PCI is not supported yet)");
        return;
    }
    usize n = (mcfg->length - sizeof(acpi::SdtHeader) - 8) / sizeof(McfgEntry);
    auto* entries = reinterpret_cast<const McfgEntry*>(reinterpret_cast<const u8*>(mcfg) + sizeof(acpi::SdtHeader) + 8);
    for (usize i = 0; i < n && g_region_count < MAX_REGIONS; i++) {
        McfgEntry e;
        memcpy(&e, &entries[i], sizeof(e));
        g_regions[g_region_count++] = EcamRegion{e.base, e.segment, e.start_bus, e.end_bus, nullptr};
    }

    // Walk the bus tree from bus 0, following PCI-to-PCI bridges, so we only
    // touch buses that exist.
    for (u32 r = 0; r < g_region_count; r++) {
        bool bus_present[256] = {};
        bus_present[g_regions[r].start_bus] = true;
        for (u32 bus = g_regions[r].start_bus; bus <= g_regions[r].end_bus; bus++) {
            if (!bus_present[bus])
                continue;
            for (u8 dev = 0; dev < 32; dev++) {
                volatile u8* cfg = config_window(r, static_cast<u8>(bus), dev, 0);
                if (!cfg || (raw_read32(cfg, 0) & 0xFFFF) == 0xFFFF)
                    continue;
                bool multi = cfg[0x0E] & 0x80;
                for (u8 fn = 0; fn < (multi ? 8 : 1); fn++) {
                    u32 before = g_count;
                    scan_function(r, static_cast<u8>(bus), dev, fn);
                    if (g_count > before) {
                        Function& f = g_functions[g_count - 1];
                        if ((f.header_type & 0x7F) == 1) { // bridge: remember its secondary bus
                            u8 secondary = read8(f, 0x19);
                            bus_present[secondary] = true;
                        }
                    }
                }
            }
        }
    }
}

void enable(const Function& f) {
    u16 cmd = read16(f, 0x04);
    cmd |= 0x7; // I/O space, memory space, bus master
    write16(f, 0x04, cmd);
}

volatile void* map_bar(const Function& f, int bar) {
    const Bar& b = f.bars[bar];
    if (!b.size || b.is_io)
        return nullptr;
    return static_cast<volatile void*>(vmalloc::map_physical(b.address, b.size));
}

bool enable_msi(Function& f, u8 vector) {
    u32 dest = percpu::get(0)->lapic_id;
    u64 address = 0xFEE00000ull | (static_cast<u64>(dest) << 12);
    if (f.msix_cap) {
        u16 control = read16(f, f.msix_cap + 2);
        u32 table = read32(f, f.msix_cap + 4);
        int bir = table & 7;
        u32 offset = table & ~7u;
        auto* base = static_cast<volatile u8*>(map_bar(f, bir));
        if (!base)
            return false;
        u32 entries = (control & 0x7FF) + 1;
        for (u32 i = 0; i < entries; i++) {
            volatile u32* e = reinterpret_cast<volatile u32*>(base + offset + i * 16);
            e[0] = static_cast<u32>(address);
            e[1] = static_cast<u32>(address >> 32);
            e[2] = vector;
            e[3] = 0; // unmasked
        }
        write16(f, f.msix_cap + 2, (control | 0x8000) & ~0x4000); // enable, function unmask
    } else if (f.msi_cap) {
        u16 control = read16(f, f.msi_cap + 2);
        bool is64 = control & 0x80;
        write32(f, f.msi_cap + 4, static_cast<u32>(address));
        if (is64) {
            write32(f, f.msi_cap + 8, static_cast<u32>(address >> 32));
            write16(f, f.msi_cap + 12, vector);
        } else {
            write16(f, f.msi_cap + 8, vector);
        }
        write16(f, f.msi_cap + 2, (control & ~0x70) | 1); // one vector, enabled
    } else {
        return false;
    }
    u16 cmd = read16(f, 0x04);
    write16(f, 0x04, cmd | 0x400); // disable legacy INTx
    return true;
}

Function* function_of(const dev::Device* d) {
    return d && d->bus == dev::Bus::Pci ? static_cast<Function*>(d->bus_data) : nullptr;
}

u32 count() { return g_count; }

const char* class_name(u8 cls, u8 sub, u8 prog_if) {
    switch (cls) {
    case 0x00: return "Unclassified device";
    case 0x01:
        switch (sub) {
        case 0x00: return "SCSI storage controller";
        case 0x01: return "IDE controller";
        case 0x04: return "RAID controller";
        case 0x05: return "ATA controller";
        case 0x06: return prog_if == 1 ? "SATA controller (AHCI)" : "SATA controller";
        case 0x07: return "SAS controller";
        case 0x08: return prog_if == 2 ? "NVM Express controller" : "Non-volatile memory controller";
        default:   return "Mass storage controller";
        }
    case 0x02: return sub == 0x80 ? "Network controller" : "Ethernet controller";
    case 0x03: return sub == 0x00 ? "VGA compatible display adapter" : "Display controller";
    case 0x04:
        switch (sub) {
        case 0x01: return "Audio device";
        case 0x03: return "High Definition Audio controller";
        default:   return "Multimedia controller";
        }
    case 0x05: return "Memory controller";
    case 0x06:
        switch (sub) {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI bridge";
        default:   return "Bridge";
        }
    case 0x07: return sub == 0x00 ? "Serial controller" : "Communication controller";
    case 0x08: return "System peripheral";
    case 0x09: return "Input device controller";
    case 0x0C:
        switch (sub) {
        case 0x03:
            switch (prog_if) {
            case 0x00: return "USB 1.1 controller (UHCI)";
            case 0x10: return "USB 1.1 controller (OHCI)";
            case 0x20: return "USB 2.0 controller (EHCI)";
            case 0x30: return "USB 3 controller (xHCI)";
            default:   return "USB controller";
            }
        case 0x05: return "SMBus controller";
        default:   return "Serial bus controller";
        }
    case 0x0D: return "Wireless controller";
    case 0x10: return "Encryption controller";
    case 0x11: return "Signal processing controller";
    default:   return "PCI device";
    }
}

} // namespace pci
