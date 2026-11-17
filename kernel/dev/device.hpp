#pragma once

#include <lib/list.hpp>
#include <lib/types.hpp>

// The device registry: every piece of hardware Syluxium knows about, from
// any bus, with the driver bound to it and its status. This is what the
// Device Manager shows, and how drivers find their hardware.
namespace dev {

enum class Bus : u8 { Platform, Acpi, Pci, Usb, Cpu };

enum class Category : u8 {
    Processor, System, Bridge, Storage, Network, Display, Audio, Input, Usb, Serial, Multimedia, Other,
};

enum class Status : u8 {
    NoDriver, // found, but nothing drives it (yet)
    Ok,       // a driver bound and started it
    Error,    // a driver tried and failed (see status_text)
};

struct Resource {
    enum Type : u8 { Mmio, Io, Irq } type;
    u64 start;
    u64 size;
};

constexpr u32 MAX_RESOURCES = 8;

struct Device {
    u32 id = 0;
    char name[96] = {};      // human-readable ("SATA AHCI controller", "Intel(R) Core(TM) ...")
    char bus_id[32] = {};    // bus address ("0000:00:1f.2", "cpu0", "ps2-keyboard")
    Bus bus = Bus::Platform;
    Category category = Category::Other;
    Device* parent = nullptr;

    u16 vendor = 0, device = 0;               // PCI/USB ids (0 if none)
    u16 subsystem_vendor = 0, subsystem_device = 0;
    u32 class_code = 0;                       // PCI: class << 16 | subclass << 8 | prog-if
    u8 revision = 0;

    const char* driver_name = nullptr;
    Status status = Status::NoDriver;
    char status_text[64] = {};

    Resource resources[MAX_RESOURCES] = {};
    u32 resource_count = 0;

    void* bus_data = nullptr;    // bus-specific info (e.g. pci::Function)
    void* driver_data = nullptr; // owned by the bound driver

    ListNode node;

    void add_resource(Resource::Type type, u64 start, u64 size) {
        if (resource_count < MAX_RESOURCES)
            resources[resource_count++] = Resource{type, start, size};
    }
};

// A driver: claims devices it recognizes and starts them.
struct Driver {
    const char* name;
    bool (*match)(const Device* d);
    long (*probe)(Device* d); // 0 = success, negative errno = failure
    ListNode node;
};

Device* add(Device* d);
void register_driver(Driver* drv);
void probe_all();            // try every registered driver on every unbound device

void for_each(void (*fn)(Device* d, void* ctx), void* ctx);
u32 count();
const char* category_name(Category c);
const char* bus_name(Bus b);

} // namespace dev
