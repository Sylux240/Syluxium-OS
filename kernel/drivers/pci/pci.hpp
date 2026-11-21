#pragma once

#include <dev/device.hpp>
#include <lib/types.hpp>

// PCI / PCI Express: enumeration through the memory-mapped configuration
// space (ECAM, from ACPI's MCFG table), BAR decoding, bus mastering and
// message-signalled interrupts (MSI / MSI-X).
namespace pci {

struct Bar {
    u64 address = 0;
    u64 size = 0;
    bool is_io = false;
    bool prefetchable = false;
    bool is_64bit = false;
};

struct Function {
    u16 segment;
    u8 bus, device, function;
    u16 vendor_id, device_id;
    u8 class_code, subclass, prog_if, revision;
    u8 header_type;
    u8 irq_line, irq_pin;
    Bar bars[6];
    u8 msi_cap = 0;   // config offsets of capabilities (0 = absent)
    u8 msix_cap = 0;
    u8 pcie_cap = 0;
    volatile u8* config = nullptr; // this function's 4 KiB ECAM window
    dev::Device* entry = nullptr; // its device-registry record
};

void init(); // enumerate everything and register it in the device registry

u8 read8(const Function& f, u16 offset);
u16 read16(const Function& f, u16 offset);
u32 read32(const Function& f, u16 offset);
// Config space of any function by address (ACPI code may touch functions
// that have no driver); nullptr if no ECAM region covers it.
volatile u8* config_space(u16 segment, u8 bus, u8 device, u8 function);
void write8(const Function& f, u16 offset, u8 value);
void write16(const Function& f, u16 offset, u16 value);
void write32(const Function& f, u16 offset, u32 value);

// Turn on memory/IO decoding and let the device DMA.
void enable(const Function& f);

// Map a memory BAR into kernel space (uncached).
volatile void* map_bar(const Function& f, int bar);

// Route the device's interrupt to `vector` on the boot CPU: MSI-X if
// available, else MSI. Returns false if the device has neither.
// prefer_msi: plain MSI when the device has both (what a single-vector
// driver that asked for MSI expects; MSI-X otherwise).
bool enable_msi(Function& f, u8 vector, bool prefer_msi = false);

Function* function_of(const dev::Device* d);
u32 count();
Function* function_at(u32 index); // 0 .. count()-1, in enumeration order

// Class/subclass -> readable name, as the Device Manager shows it.
const char* class_name(u8 class_code, u8 subclass, u8 prog_if);

} // namespace pci
