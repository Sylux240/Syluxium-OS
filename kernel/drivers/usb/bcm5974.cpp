// Apple USB multi-touch trackpads ("Wellspring", Broadcom BCM5974 family)
// of MacBook Airs/Pros 2008-2015. The trackpad is a HID-class interface
// (protocol "mouse") whose interrupt endpoint sends Apple's own format: a
// header, then one record per finger (little-endian 16-bit fields:
// origin, x, y, relative x/y, tool and touch size, orientation, ...).
// Models before June 2013 (and the 2015 MacBook Pro) start as a plain HID
// mouse and must be switched to this "Wellspring mode" with a vendor
// request first.
// Protocol facts as documented by Linux's bcm5974 driver; our own code.
// Fingers are turned into pointer/scroll/click events (TouchpadMouse).

#include <core/log.hpp>
#include <drivers/input/touchpad.hpp>
#include <drivers/usb/usb.hpp>
#include <mm/heap.hpp>

namespace usb::bcm5974 {

namespace {

enum Type : u8 { TYPE1 = 1, TYPE2, TYPE3, TYPE4 };

struct Model {
    u16 products[3];  // ANSI, ISO, JIS keyboard variants
    Type type;
    i32 x_min, x_max, y_min, y_max;
    const char* name;
};

// Linux's bcm5974_config_table.
const Model MODELS[] = {
    {{0x0223, 0x0224, 0x0225}, TYPE1, -4824, 5342, -172, 5820, "MacBook Air 2008"},
    {{0x0230, 0x0231, 0x0232}, TYPE1, -4824, 4824, -172, 4290, "MacBook Pro 2008 (Penryn)"},
    {{0x0236, 0x0237, 0x0238}, TYPE2, -4460, 5166, -75, 6700, "MacBook 5,1 (unibody)"},
    {{0x023f, 0x0240, 0x0241}, TYPE2, -4620, 5140, -150, 6600, "MacBook Air 3,2"},
    {{0x0242, 0x0243, 0x0244}, TYPE2, -4616, 5112, -142, 5234, "MacBook Air 3,1"},
    {{0x0245, 0x0246, 0x0247}, TYPE2, -4415, 5050, -55, 6680, "MacBook Pro 8 (2011)"},
    {{0x024c, 0x024d, 0x024e}, TYPE2, -4620, 5140, -150, 6600, "MacBook Air 4,2"},
    {{0x0252, 0x0253, 0x0254}, TYPE2, -4750, 5280, -150, 6730, "MacBook Pro 8,2"},
    {{0x0249, 0x024a, 0x024b}, TYPE2, -4620, 5140, -150, 6600, "MacBook Air 4,1"},
    {{0x0262, 0x0263, 0x0264}, TYPE2, -4750, 5280, -150, 6730, "MacBook Pro 10,1 (2012)"},
    {{0x0259, 0x025a, 0x025b}, TYPE2, -4750, 5280, -150, 6730, "MacBook Pro 10,2 (2012)"},
    {{0x0290, 0x0291, 0x0292}, TYPE3, -4620, 5140, -150, 6600, "MacBook Air 2013-2015"},
    {{0x0272, 0x0273, 0x0274}, TYPE4, -4828, 5345, -203, 6803, "MacBook Pro 12,1 (2015)"},
};

struct Format {
    u16 header, button, finger_size, delta; // bytes
    u8 endpoint;
    // Mode switch: a class request reads `size` bytes of report `value`
    // from interface `index`, byte `switch_index` becomes `on`, written back.
    u16 um_size, um_value, um_index, um_switch_index;
    u8 um_on;
};

Format format_of(Type t) {
    switch (t) {
    case TYPE1: return {13 * 2, 0, 14 * 2, 0, 0x81, 8, 0x300, 0, 0, 0x01};
    case TYPE2: return {15 * 2, 15, 14 * 2, 0, 0x81, 8, 0x300, 0, 0, 0x01};
    case TYPE3: return {19 * 2, 23, 14 * 2, 0, 0x83, 0, 0, 0, 0, 0};
    case TYPE4: default: return {23 * 2, 31, 15 * 2, 2, 0x83, 2, 0x302, 2, 1, 0x01};
    }
}

constexpr int MAX_FINGERS = 16;
constexpr u8 REQ_MODE_READ = 1, REQ_MODE_WRITE = 9;

struct Trackpad {
    const Model* model;
    Format fmt;
    input::TouchpadMouse mouse;
    u32 reports = 0;
};

i32 le16s(const u8* p) { return static_cast<i16>(p[0] | (p[1] << 8)); }

void report(void* ctx, const u8* data, u32 length) {
    auto* t = static_cast<Trackpad*>(ctx);
    const Format& f = t->fmt;
    if (t->reports++ < 2)
        klog::info("bcm5974: report of %u bytes", length);
    if (length == 2)
        return; // the answer to a control request
    if (length < f.header || (length - f.header) % f.finger_size != 0)
        return;
    int raw = static_cast<int>((length - f.header) / f.finger_size);
    input::TouchpadMouse::Finger fingers[MAX_FINGERS];
    int n = 0;
    for (int i = 0; i < raw && n < MAX_FINGERS; i++) {
        const u8* r = data + f.header + f.delta + i * f.finger_size;
        if (r + 18 > data + length)
            break;
        if (le16s(r + 16) == 0) // touch_major: 0 = not touching
            continue;
        // Apple's y already grows upwards (towards the keyboard), which is
        // what TouchpadMouse expects (Linux flips it for screen coordinates).
        fingers[n++] = {le16s(r + 2), le16s(r + 4)};
    }
    // Type 1 has separate buttons (another endpoint, not used): taps only.
    bool button = t->model->type != TYPE1 && f.button < length && data[f.button] != 0;
    t->mouse.frame(fingers, n, button);
}

// An interrupt IN endpoint anywhere in the configuration, except on a HID
// keyboard interface (the keyboard's own reports). Its interface in *ifnum.
bool find_endpoint(Device* d, u8 address, Endpoint* out, u8* ifnum) {
    const u8* c = d->config_desc;
    bool keyboard = false;
    u8 current = 0, alt = 0;
    for (u16 off = 0; off + 2 <= d->config_len && c[off] >= 2; off = static_cast<u16>(off + c[off])) {
        if (c[off + 1] == 4 && c[off] >= 9) { // interface
            current = c[off + 2];
            alt = c[off + 3];
            keyboard = c[off + 5] == 3 && c[off + 7] == 1;
        } else if (c[off + 1] == 5 && c[off] >= 7 && alt == 0 && !keyboard && c[off + 2] == address &&
                   (c[off + 3] & 3) == 3) { // its interrupt endpoint
            *out = Endpoint{c[off + 2], c[off + 3], static_cast<u16>(c[off + 4] | (c[off + 5] << 8)), c[off + 6]};
            *ifnum = current;
            return true;
        }
    }
    return false;
}

// Switch the trackpad from HID-mouse mode to multi-touch reports.
bool wellspring_mode(Device* d, const Format& f) {
    if (!f.um_size)
        return true; // type 3: always in that mode
    u8 buf[8] = {};
    long n = d->control(0xA1, REQ_MODE_READ, f.um_value, f.um_index, buf, f.um_size);
    if (n != f.um_size) {
        klog::warn("bcm5974: reading the trackpad mode failed (%ld)", n);
        return false;
    }
    buf[f.um_switch_index] = f.um_on;
    n = d->control(0x21, REQ_MODE_WRITE, f.um_value, f.um_index, buf, f.um_size);
    if (n < 0) {
        klog::warn("bcm5974: switching the trackpad to multi-touch mode failed (%ld)", n);
        return false;
    }
    return true;
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
    Format fmt = format_of(model->type);
    const Endpoint* ep = nullptr;
    for (int i = 0; i < intf.endpoint_count; i++)
        if (intf.endpoints[i].address == fmt.endpoint)
            ep = &intf.endpoints[i];
    // Some models (MacBookPro9,x: 05ac:0252 with the trackpad as interface
    // 2) send the multi-touch reports on another interface's endpoint, the
    // vendor-defined one: Linux reads the endpoint wherever it is.
    Endpoint other{};
    u8 other_if = 0;
    if (!ep && find_endpoint(d, fmt.endpoint, &other, &other_if)) {
        klog::info("bcm5974: %s: trackpad endpoint %#x is on interface %u (trackpad interface %u)", model->name,
                   fmt.endpoint, other_if, intf.number);
        ep = &other;
    }
    if (!ep) {
        klog::warn("bcm5974: %s: trackpad endpoint %#x not on interface %u", model->name, fmt.endpoint, intf.number);
        return false;
    }
    if (!wellspring_mode(d, fmt))
        return false; // left to the HID driver: at least a plain mouse
    if (d->open_endpoint(*ep) != 0)
        return false;
    auto* t = new Trackpad;
    t->model = model;
    t->fmt = fmt;
    // ~9800 units across a ~105 mm pad.
    t->mouse.init("Apple Internal Trackpad", input::BUS_USB, d->vendor, d->product,
                  (model->x_max - model->x_min) / 105);
    // Integrated button (types 2-4): the whole pad clicks.
    t->mouse.set_area(model->x_min, model->x_max, model->y_min, model->y_max, false);
    d->add_binding([](void* ctx) { input::unregister_device(&static_cast<Trackpad*>(ctx)->mouse.device()); }, t);
    u32 length = fmt.header + fmt.delta + fmt.finger_size * MAX_FINGERS;
    if (length < ep->max_packet)
        length = ep->max_packet;
    d->start_interrupt_in(ep->address, length, report, t);
    klog::ok("bcm5974: %s trackpad on interface %u (type %u: pointer, two-finger scroll, tap and click)", model->name,
             intf.number, model->type);
    return true;
}

} // namespace usb::bcm5974
