// Apple USB multi-touch trackpads ("Wellspring", Broadcom BCM5974 family)
// of MacBook Airs/Pros. The trackpad is a HID-class interface (protocol
// "mouse") whose interrupt endpoint sends Apple's own format: a header,
// then one 28-byte record per finger (little-endian 16-bit fields:
// origin, x, y, relative x/y, tool and touch size, orientation, ...).
// Protocol facts as documented by Linux's bcm5974 driver; our own code.
// Fingers are turned into pointer/scroll/click events (TouchpadMouse).

#include <core/log.hpp>
#include <drivers/input/touchpad.hpp>
#include <drivers/usb/usb.hpp>

namespace usb::bcm5974 {

namespace {

struct Model {
    u16 products[3];  // ANSI, ISO, JIS keyboard variants
    u8 endpoint;      // trackpad interrupt IN endpoint
    u16 header;       // bytes before the first finger record
    u16 button;       // offset of the integrated button state
    u16 finger_size;
    i32 x_min, x_max, y_min, y_max;
    const char* name;
};

// Type 3 (June 2013 on): no mode switch needed.
const Model MODELS[] = {
    {{0x0290, 0x0291, 0x0292}, 0x83, 19 * 2, 23, 14 * 2, -4620, 5140, -150, 6600, "MacBook Air 2013-2015"},
};

constexpr int MAX_FINGERS = 16;

struct Trackpad {
    const Model* model;
    input::TouchpadMouse mouse;
    u32 reports = 0;
};

i32 le16s(const u8* p) { return static_cast<i16>(p[0] | (p[1] << 8)); }

void report(void* ctx, const u8* data, u32 length) {
    auto* t = static_cast<Trackpad*>(ctx);
    const Model& m = *t->model;
    if (t->reports++ < 2)
        klog::info("bcm5974: report of %u bytes", length);
    if (length < m.header || (length - m.header) % m.finger_size != 0)
        return;
    int raw = static_cast<int>((length - m.header) / m.finger_size);
    input::TouchpadMouse::Finger fingers[MAX_FINGERS];
    int n = 0;
    for (int i = 0; i < raw && n < MAX_FINGERS; i++) {
        const u8* f = data + m.header + i * m.finger_size;
        if (le16s(f + 16) == 0) // touch_major: 0 = not touching
            continue;
        // Apple's y grows downwards from the top edge; flip so it grows up.
        fingers[n++] = {le16s(f + 2), m.y_min + m.y_max - le16s(f + 4)};
    }
    bool button = data[m.button] != 0;
    t->mouse.frame(fingers, n, button);
}

} // namespace

bool probe(Device* d, const Interface& intf) {
    if (d->vendor != 0x05AC || intf.cls != 3 || intf.protocol != 2)
        return false;
    const Model* model = nullptr;
    for (const Model& m : MODELS)
        for (u16 p : m.products)
            if (p == d->product)
                model = &m;
    if (!model)
        return false;
    const Endpoint* ep = nullptr;
    for (int i = 0; i < intf.endpoint_count; i++)
        if (intf.endpoints[i].address == model->endpoint)
            ep = &intf.endpoints[i];
    if (!ep || d->open_endpoint(*ep) != 0)
        return false;
    auto* t = new Trackpad;
    t->model = model;
    // ~9760 units across a ~105 mm pad.
    t->mouse.init("Apple Internal Trackpad", input::BUS_USB, d->vendor, d->product,
                  (model->x_max - model->x_min) / 105);
    d->add_binding([](void* ctx) { input::unregister_device(&static_cast<Trackpad*>(ctx)->mouse.device()); }, t);
    u32 length = model->header + model->finger_size * MAX_FINGERS;
    d->start_interrupt_in(ep->address, length, report, t);
    klog::ok("bcm5974: %s trackpad on interface %u (pointer, two-finger scroll, tap and click)", model->name,
             intf.number);
    return true;
}

} // namespace usb::bcm5974
