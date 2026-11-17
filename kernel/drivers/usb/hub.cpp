// USB 2.0 hub class driver: powers the hub's ports, resets whatever is
// connected and hands each device to the host controller and the USB core.
// Hubs inside hubs work the same way (up to 5 tiers). Hot-plug through the
// hub's status-change endpoint comes with general USB hot-plug.

#include <core/log.hpp>
#include <drivers/usb/usb.hpp>
#include <lib/errno.hpp>
#include <sched/sched.hpp>
#include <time/clock.hpp>

namespace usb::hub {

namespace {

constexpr u8 REQ_GET_STATUS = 0, REQ_CLEAR_FEATURE = 1, REQ_SET_FEATURE = 3;
constexpr u16 PORT_RESET = 4, PORT_POWER = 8, C_PORT_CONNECTION = 16, C_PORT_RESET = 20;
constexpr u16 STATUS_CONNECTION = 1 << 0, STATUS_ENABLE = 1 << 1, STATUS_LOW_SPEED = 1 << 9,
              STATUS_HIGH_SPEED = 1 << 10;

bool port_status(Device* d, u8 port, u16* status, u16* change) {
    u8 buf[4];
    if (d->control(0xA3, REQ_GET_STATUS, 0, port, buf, 4) != 4)
        return false;
    *status = static_cast<u16>(buf[0] | (buf[1] << 8));
    *change = static_cast<u16>(buf[2] | (buf[3] << 8));
    return true;
}

} // namespace

bool probe(Device* d, const Interface& intf) {
    if (intf.cls != 9)
        return false;
    u8 desc[16];
    if (d->control(0xA0, REQ_GET_DESCRIPTOR, 0x29 << 8, 0, desc, sizeof(desc)) < 7)
        return false;
    u8 ports = desc[2];
    u16 characteristics = static_cast<u16>(desc[3] | (desc[4] << 8));
    u32 power_on_ms = desc[5] * 2u;
    if (!ports || ports > 15)
        return false;
    // Protocol 2 = high-speed hub with one transaction translator per port.
    if (d->set_hub(ports, intf.protocol == 2, static_cast<u8>((characteristics >> 5) & 3)) != 0)
        return false;

    for (u8 p = 1; p <= ports; p++)
        d->control(0x23, REQ_SET_FEATURE, PORT_POWER, p, nullptr, 0);
    sched::sleep_ms(power_on_ms > 100 ? power_on_ms : 100);

    for (u8 p = 1; p <= ports; p++) {
        u16 status, change;
        if (!port_status(d, p, &status, &change) || !(status & STATUS_CONNECTION))
            continue;
        d->control(0x23, REQ_CLEAR_FEATURE, C_PORT_CONNECTION, p, nullptr, 0);
        d->control(0x23, REQ_SET_FEATURE, PORT_RESET, p, nullptr, 0);
        u64 deadline = clock::now_ns() + 500'000'000;
        bool reset_done = false;
        while (clock::now_ns() < deadline) {
            sched::sleep_ms(10);
            if (port_status(d, p, &status, &change) && (change & (1 << 4))) { // C_PORT_RESET
                reset_done = true;
                break;
            }
        }
        d->control(0x23, REQ_CLEAR_FEATURE, C_PORT_RESET, p, nullptr, 0);
        if (!reset_done || !(status & STATUS_ENABLE))
            continue;
        sched::sleep_ms(10); // reset recovery
        // xHCI speed IDs: 1 full, 2 low, 3 high.
        u8 speed = (status & STATUS_LOW_SPEED) ? 2 : (status & STATUS_HIGH_SPEED) ? 3 : 1;
        Device* child = d->add_child(p, speed);
        if (!child) {
            klog::warn("usb: device on hub port %u could not be addressed", p);
            continue;
        }
        child->controller = d->entry;
        attach(child);
    }
    return true;
}

} // namespace usb::hub
