#pragma once

#include <dev/device.hpp>
#include <lib/types.hpp>

// USB core: what class drivers (HID, mass storage, later audio, video,
// gamepads, Wi-Fi dongles) see of a device, independent of the host
// controller type.
namespace usb {

struct Endpoint {
    u8 address;     // bit 7: IN
    u8 attributes;  // bits 0-1: 0 control, 1 isochronous, 2 bulk, 3 interrupt
    u16 max_packet;
    u8 interval;
    bool is_in() const { return address & 0x80; }
    u8 type() const { return attributes & 3; }
};

struct Interface {
    u8 number, alt_setting, cls, subclass, protocol;
    Endpoint endpoints[8];
    int endpoint_count;
};

using InterruptCallback = void (*)(void* ctx, const u8* data, u32 length);

class Device {
public:
    virtual ~Device() = default;

    // Control transfer on endpoint 0. Returns bytes transferred or -errno.
    virtual long control(u8 request_type, u8 request, u16 value, u16 index, void* data, u16 length) = 0;

    // Make an endpoint usable (after SET_CONFIGURATION).
    virtual long open_endpoint(const Endpoint& ep) = 0;

    // Synchronous bulk/interrupt transfer; returns bytes transferred or -errno.
    virtual long transfer(u8 ep_address, void* data, u32 length) = 0;

    // Keep an interrupt IN endpoint polled; `cb` runs for every report.
    virtual long start_interrupt_in(u8 ep_address, u32 length, InterruptCallback cb, void* ctx) = 0;

    // Hubs: tell the host controller this device is a hub, then create the
    // device that appeared on one of its ports (already reset).
    virtual long set_hub(u8 ports, bool multi_tt, u8 think_time) = 0;
    virtual Device* add_child(u8 hub_port, u8 child_speed) = 0;

    u16 vendor = 0, product = 0;
    u8 speed = 0; // 1 full, 2 low, 3 high, 4 super
    char path[24] = {};   // Linux-style topology: "1-3" (bus 1, port 3), "1-3.2" (port 2 of that hub)
    char product_name[64] = {};
    dev::Device* entry = nullptr;   // device-registry record
    dev::Device* controller = nullptr;
};

// Called by the host controller driver once a device is addressed:
// reads descriptors, configures it and binds class drivers.
void attach(Device* d);

// Class drivers.
namespace hid {
bool probe(Device* d, const Interface& intf);
}
namespace storage {
bool probe(Device* d, const Interface& intf);
}
namespace hub {
bool probe(Device* d, const Interface& intf);
}

constexpr u8 REQ_GET_DESCRIPTOR = 6, REQ_SET_CONFIGURATION = 9;
constexpr u8 DESC_DEVICE = 1, DESC_CONFIGURATION = 2, DESC_STRING = 3, DESC_INTERFACE = 4, DESC_ENDPOINT = 5;

} // namespace usb

namespace xhci {
void register_driver();
// Enumerate devices on every controller's ports. Called after all PCI
// drivers are probed, so built-in disks get the first sdX names.
void enumerate_all();
}
