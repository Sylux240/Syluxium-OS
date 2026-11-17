// USB hub class driver (USB 2.0 and USB 3 SuperSpeed hubs): powers the
// hub's ports, resets whatever is connected and hands each device to the
// host controller and the USB core. Hubs inside hubs work the same way (up
// to 5 tiers).
//
// Hot-plug: the hub reports port changes on its interrupt IN endpoint as a
// bitmap (bit 0 = the hub itself, bit N = port N). The callback (host
// controller event thread) only records the bits; the USB worker thread then
// asks the hub what happened and enumerates or detaches devices.
//
// A USB 3 hub is really two hubs in one package: a USB 2 hub on the USB 2
// wires and a SuperSpeed hub on the SuperSpeed ones, each enumerated
// separately. The SuperSpeed half has its own descriptor (type 0x2A), must be
// told its depth in the tree (SET_HUB_DEPTH, used for route strings), has a
// different port status layout, and only ever has SuperSpeed children.

#include <core/log.hpp>
#include <drivers/usb/usb.hpp>
#include <lib/errno.hpp>
#include <lib/string.hpp>
#include <sched/sched.hpp>
#include <time/clock.hpp>

namespace usb::hub {

namespace {

constexpr u8 REQ_SET_HUB_DEPTH = 12;
// Port features (wValue of SET/CLEAR_FEATURE to a port).
constexpr u16 PORT_RESET = 4, PORT_POWER = 8, C_PORT_CONNECTION = 16, C_PORT_ENABLE = 17, C_PORT_SUSPEND = 18,
              C_PORT_OVER_CURRENT = 19, C_PORT_RESET = 20, C_PORT_LINK_STATE = 25, C_PORT_CONFIG_ERROR = 26,
              C_BH_PORT_RESET = 29;
// Hub features (wValue of CLEAR_FEATURE to the hub).
constexpr u16 C_HUB_LOCAL_POWER = 0, C_HUB_OVER_CURRENT = 1;
// wPortStatus bits (USB 2 and USB 3 agree on these two).
constexpr u16 STATUS_CONNECTION = 1 << 0, STATUS_ENABLE = 1 << 1;
// USB 2 only: device speed.
constexpr u16 STATUS_LOW_SPEED = 1 << 9, STATUS_HIGH_SPEED = 1 << 10;
// wPortChange bits.
constexpr u16 CHANGE_CONNECTION = 1 << 0, CHANGE_RESET = 1 << 4;

constexpr u8 MAX_PORTS = 15;

struct Hub {
    Device* dev = nullptr;
    u8 ports = 0;
    bool superspeed = false;
    Device* children[MAX_PORTS + 1] = {};
    u32 pending = 0;      // change bitmap from the status-change endpoint
    bool queued = false;  // a hub_work item is waiting
    bool gone = false;    // unplugged: the struct is never freed (work items may still name it)
};

bool port_status(Hub* h, u8 port, u16* status, u16* change) {
    u8 buf[4];
    if (h->dev->control(0xA3, REQ_GET_STATUS, 0, port, buf, 4) != 4)
        return false;
    *status = static_cast<u16>(buf[0] | (buf[1] << 8));
    *change = static_cast<u16>(buf[2] | (buf[3] << 8));
    return true;
}

// Acknowledge every change bit the hub reported for a port.
void clear_changes(Hub* h, u8 port, u16 change) {
    static constexpr u16 USB2_FEATURES[5] = {C_PORT_CONNECTION, C_PORT_ENABLE, C_PORT_SUSPEND, C_PORT_OVER_CURRENT,
                                             C_PORT_RESET};
    static constexpr u16 USB3_FEATURES[8] = {C_PORT_CONNECTION, 0,                 0, C_PORT_OVER_CURRENT,
                                             C_PORT_RESET,      C_BH_PORT_RESET, C_PORT_LINK_STATE,
                                             C_PORT_CONFIG_ERROR};
    const u16* features = h->superspeed ? USB3_FEATURES : USB2_FEATURES;
    int count = h->superspeed ? 8 : 5;
    for (int bit = 0; bit < count; bit++)
        if ((change & (1 << bit)) && (bit == 0 || features[bit]))
            h->dev->control(0x23, REQ_CLEAR_FEATURE, features[bit], port, nullptr, 0);
}

// Reset the port and wait until it's enabled; false if nothing usable is there.
bool reset_port(Hub* h, u8 port, u16* status) {
    u16 change;
    h->dev->control(0x23, REQ_SET_FEATURE, PORT_RESET, port, nullptr, 0);
    u64 deadline = clock::now_ns() + 500'000'000;
    bool done = false;
    while (clock::now_ns() < deadline) {
        sched::sleep_ms(10);
        if (port_status(h, port, status, &change) && (change & CHANGE_RESET)) {
            done = true;
            break;
        }
    }
    h->dev->control(0x23, REQ_CLEAR_FEATURE, C_PORT_RESET, port, nullptr, 0);
    if (h->superspeed)
        h->dev->control(0x23, REQ_CLEAR_FEATURE, C_BH_PORT_RESET, port, nullptr, 0);
    if (!done || !(*status & STATUS_ENABLE))
        return false;
    sched::sleep_ms(10); // reset recovery
    return true;
}

void connect_port(Hub* h, u8 port, bool hotplug) {
    if (hotplug) {
        sched::sleep_ms(100); // debounce (USB 2.0 7.1.7.3)
        u16 status, change;
        if (!port_status(h, port, &status, &change) || !(status & STATUS_CONNECTION))
            return;
    }
    u16 status;
    if (!reset_port(h, port, &status))
        return;
    // xHCI speed IDs: 1 full, 2 low, 3 high, 4 super. A SuperSpeed hub only
    // carries SuperSpeed devices; a USB 2 hub reports the speed per port.
    u8 speed = h->superspeed ? 4 : (status & STATUS_LOW_SPEED) ? 2 : (status & STATUS_HIGH_SPEED) ? 3 : 1;
    Device* child = h->dev->add_child(port, speed);
    if (!child) {
        klog::warn("usb: device on hub %s port %u could not be addressed", h->dev->path, port);
        return;
    }
    child->controller = h->dev->entry;
    h->children[port] = child;
    attach(child);
}

// Look at one port: detach what's gone, enumerate what's new. Topology lock held.
void check_port(Hub* h, u8 port, bool hotplug) {
    u16 status, change;
    if (!port_status(h, port, &status, &change))
        return;
    clear_changes(h, port, change);
    bool connected = status & STATUS_CONNECTION;
    Device* child = h->children[port];
    if (child && (!connected || (change & CHANGE_CONNECTION))) {
        detach(child);
        h->children[port] = nullptr;
    }
    if (connected && !h->children[port])
        connect_port(h, port, hotplug);
}

// USB worker: the hub reported changes.
void hub_work(void* arg) {
    auto* h = static_cast<Hub*>(arg);
    __atomic_store_n(&h->queued, false, __ATOMIC_RELEASE);
    u32 bits = __atomic_exchange_n(&h->pending, 0u, __ATOMIC_ACQ_REL);
    if (h->gone)
        return;
    if (bits & 1) { // the hub itself: power / over-current changes
        u8 buf[4];
        if (h->dev->control(0xA0, REQ_GET_STATUS, 0, 0, buf, 4) == 4) {
            u16 change = static_cast<u16>(buf[2] | (buf[3] << 8));
            if (change & 1)
                h->dev->control(0x20, REQ_CLEAR_FEATURE, C_HUB_LOCAL_POWER, 0, nullptr, 0);
            if (change & 2)
                h->dev->control(0x20, REQ_CLEAR_FEATURE, C_HUB_OVER_CURRENT, 0, nullptr, 0);
        }
    }
    for (u8 p = 1; p <= h->ports; p++)
        if (bits & (1u << p))
            check_port(h, p, true);
}

// Status-change endpoint (host controller event thread: must not sleep).
void status_change(void* ctx, const u8* data, u32 length) {
    auto* h = static_cast<Hub*>(ctx);
    u32 bits = 0;
    for (u32 i = 0; i < length && i < 4; i++)
        bits |= static_cast<u32>(data[i]) << (8 * i);
    if (!bits || h->gone)
        return;
    __atomic_or_fetch(&h->pending, bits, __ATOMIC_RELEASE);
    if (!__atomic_exchange_n(&h->queued, true, __ATOMIC_ACQ_REL))
        queue_work(hub_work, h);
}

// The hub was unplugged: everything behind it goes too. Topology lock held.
void disconnect(void* ctx) {
    auto* h = static_cast<Hub*>(ctx);
    h->gone = true;
    for (u8 p = 1; p <= h->ports; p++) {
        if (h->children[p]) {
            detach(h->children[p]);
            h->children[p] = nullptr;
        }
    }
}

} // namespace

bool probe(Device* d, const Interface& intf) {
    if (intf.cls != 9)
        return false;
    bool superspeed = d->speed >= 4;
    u8 desc[16];
    long n = superspeed ? d->control(0xA0, REQ_GET_DESCRIPTOR, DESC_SS_HUB << 8, 0, desc, 12)
                        : d->control(0xA0, REQ_GET_DESCRIPTOR, DESC_HUB << 8, 0, desc, sizeof(desc));
    if (n < 7)
        return false;
    u8 ports = desc[2];
    u16 characteristics = static_cast<u16>(desc[3] | (desc[4] << 8));
    u32 power_on_ms = desc[5] * 2u;
    if (!ports || ports > MAX_PORTS)
        return false;
    if (superspeed) {
        // The hub needs its tier (0 = attached to a root port) to pick its
        // part of the route string out of packet headers.
        if (d->control(0x20, REQ_SET_HUB_DEPTH, d->hub_depth, 0, nullptr, 0) < 0)
            return false;
        if (d->set_hub(ports, false, 0) != 0)
            return false;
    } else if (d->set_hub(ports, intf.protocol == 2, static_cast<u8>((characteristics >> 5) & 3)) != 0) {
        // Protocol 2 = high-speed hub with one transaction translator per port.
        return false;
    }

    auto* h = new Hub;
    h->dev = d;
    h->ports = ports;
    h->superspeed = superspeed;
    d->add_binding(disconnect, h);

    for (u8 p = 1; p <= ports; p++)
        d->control(0x23, REQ_SET_FEATURE, PORT_POWER, p, nullptr, 0);
    sched::sleep_ms(power_on_ms > 100 ? power_on_ms : 100);
    for (u8 p = 1; p <= ports; p++)
        check_port(h, p, false);

    // From now on the hub tells us about changes.
    const Endpoint* ep = nullptr;
    for (int i = 0; i < intf.endpoint_count; i++)
        if (intf.endpoints[i].type() == 3 && intf.endpoints[i].is_in())
            ep = &intf.endpoints[i];
    if (!ep || d->open_endpoint(*ep) != 0 ||
        d->start_interrupt_in(ep->address, (ports + 1 + 7) / 8, status_change, h) != 0)
        klog::warn("usb: hub %s has no status-change endpoint: no hot-plug on it", d->path);
    klog::info("USB %s: %s hub with %u ports", d->path, superspeed ? "SuperSpeed" : "USB 2", ports);
    return true;
}

} // namespace usb::hub
