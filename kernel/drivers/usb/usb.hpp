#pragma once

#include <dev/device.hpp>
#include <lib/types.hpp>
#include <sched/sync.hpp>

// USB core: what class drivers (HID, mass storage, later audio, video,
// gamepads, Wi-Fi dongles) see of a device, independent of the host
// controller type.
//
// Hot-plug: devices come and go at any time. Host controller drivers report
// port changes (root ports; hubs report theirs through their status-change
// endpoint) to the "usb-hub" worker thread, which enumerates new devices and
// detaches removed ones. Everything that changes the device tree runs with
// topology_lock() held, one thing at a time (like Linux's khubd).
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
    u16 hid_report_length; // HID: size of the report descriptor (from the HID class descriptor)
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

    // Keep an interrupt IN endpoint polled; `cb` runs for every report (in
    // the host controller's event thread: it must not sleep).
    virtual long start_interrupt_in(u8 ep_address, u32 length, InterruptCallback cb, void* ctx) = 0;

    // Hubs: tell the host controller this device is a hub, then create the
    // device that appeared on one of its ports (already reset). Speeds use
    // the xHCI numbering: 1 full, 2 low, 3 high, 4 super.
    virtual long set_hub(u8 ports, bool multi_tt, u8 think_time) = 0;
    virtual Device* add_child(u8 hub_port, u8 child_speed) = 0;

    // Unplugged: stop every transfer (waiters fail with ENODEV, interrupt
    // callbacks have stopped when this returns) and give the host
    // controller's resources back. The object itself stays valid - class
    // drivers may still hold the pointer - but every request fails.
    virtual void disable() = 0;

    // Class drivers register what to undo when the device goes away.
    // Called (in reverse order) by detach(), after disable().
    void add_binding(void (*disconnect)(void* ctx), void* ctx) {
        if (binding_count < MAX_BINDINGS)
            bindings[binding_count++] = Binding{disconnect, ctx};
    }

    u16 vendor = 0, product = 0;
    u8 speed = 0; // 1 full, 2 low, 3 high, 4 super
    u8 hub_depth = 0; // 0: on a root port
    char path[24] = {};   // Linux-style topology: "1-3" (bus 1, port 3), "1-3.2" (port 2 of that hub)
    char product_name[64] = {};
    dev::Device* entry = nullptr;   // device-registry record
    dev::Device* controller = nullptr;
    volatile bool gone = false;     // unplugged: requests fail

    struct Binding {
        void (*disconnect)(void* ctx);
        void* ctx;
    };
    static constexpr int MAX_BINDINGS = 8;
    Binding bindings[MAX_BINDINGS] = {};
    int binding_count = 0;
};

// Called by the host controller driver once a device is addressed:
// reads descriptors, configures it and binds class drivers.
void attach(Device* d);

// The device was unplugged (or its hub was): disable it, run the class
// drivers' disconnect hooks (a hub detaches its children first) and drop
// it from the device registry. Topology lock held.
void detach(Device* d);

// Serializes enumeration and removal.
sched::Mutex& topology_lock();

// Run fn(arg) on the usb-hub worker thread, with the topology lock held.
// Safe from interrupt callbacks (the controllers' event threads).
void queue_work(void (*fn)(void*), void* arg);

// Start the worker (after the boot-time scan of all ports).
void start_hotplug();

// Class drivers.
namespace hid {
bool probe(Device* d, const Interface& intf);
void run_selftest(); // report-descriptor parser + input mapping, with real descriptors
}
namespace xpad {
bool probe(Device* d, const Interface& intf); // Xbox 360 / One controllers (vendor class)
void run_selftest();
}
namespace storage {
bool probe(Device* d, const Interface& intf);
}
namespace hub {
bool probe(Device* d, const Interface& intf);
}

constexpr u8 REQ_GET_STATUS = 0, REQ_CLEAR_FEATURE = 1, REQ_SET_FEATURE = 3, REQ_GET_DESCRIPTOR = 6,
             REQ_SET_CONFIGURATION = 9;
constexpr u8 DESC_DEVICE = 1, DESC_CONFIGURATION = 2, DESC_STRING = 3, DESC_INTERFACE = 4, DESC_ENDPOINT = 5,
             DESC_HID = 0x21, DESC_HID_REPORT = 0x22, DESC_HUB = 0x29, DESC_SS_HUB = 0x2A;

} // namespace usb

namespace xhci {
void register_driver();
// Enumerate devices on every controller's ports. Called after all PCI
// drivers are probed, so built-in disks get the first sdX names.
void enumerate_all();
}
