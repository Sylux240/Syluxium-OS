// USB HID class driver.
//
// Every HID interface is driven in report protocol: its report descriptor
// is parsed (hid_parser.cpp) and reports are translated into Linux input
// events, one /dev/input/eventN per top-level application collection - mice
// with any number of buttons and wheels, tablets/absolute pointers,
// keyboards incl. media keys, gamepads and joysticks. Keyboards also feed the
// text console. If a boot keyboard/mouse comes with a descriptor we can't
// use, the fixed boot-protocol report format is the fallback.

#include <core/log.hpp>
#include <drivers/input/input.hpp>
#include <drivers/ps2kbd.hpp>
#include <drivers/usb/hid_parser.hpp>
#include <drivers/usb/usb.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>

namespace usb::hid {

namespace {

constexpr u8 REQ_SET_IDLE = 0x0A, REQ_SET_PROTOCOL = 0x0B;
constexpr u16 PROTOCOL_BOOT = 0, PROTOCOL_REPORT = 1;

void name_device(input::Device& in, Device* d, const char* suffix) {
    snprintf(in.name, sizeof(in.name), "%s%s%s", d->product_name, suffix[0] ? " " : "", suffix);
    in.bustype = input::BUS_USB;
    in.vendor = d->vendor;
    in.product = d->product;
}

// Linux names the extra devices of a multi-application interface after the application.
const char* suffix_for(u32 app, int count) {
    if (count <= 1)
        return "";
    switch (app) {
    case GD_MOUSE: return "Mouse";
    case GD_KEYBOARD: return "Keyboard";
    case GD_KEYPAD: return "Keypad";
    case GD_SYSTEM_CONTROL: return "System Control";
    case CONSUMER_CONTROL: return "Consumer Control";
    case GD_GAMEPAD: case GD_JOYSTICK: return "Gamepad";
    case DG_PEN: return "Pen";
    case DG_TOUCHSCREEN: return "Touchscreen";
    default: return "";
    }
}

// ---- report protocol ----

class ReportDevice final : public EventSink {
public:
    void event(int app, u16 type, u16 code, i32 value) override {
        hid.devices[app].report(type, code, value);
        // Keyboard keys also go to the text console (codes below BTN_MISC are keys).
        if (hid.is_keyboard[app] && type == input::EV_KEY && code < input::BTN_MISC)
            ps2kbd::console_key(code, value != 0);
    }

    HidInput hid;
};

void report_received(void* ctx, const u8* data, u32 length) {
    static_cast<ReportDevice*>(ctx)->hid.process(data, length);
}

void report_disconnect(void* ctx) {
    auto* r = static_cast<ReportDevice*>(ctx);
    for (int i = 0; i < r->hid.device_count; i++)
        if (r->hid.device_used[i])
            input::unregister_device(&r->hid.devices[i]);
    delete r;
}

// ---- boot protocol fallback ----

struct Keyboard {
    input::Device input;
    u8 last[8] = {};
};

struct Mouse {
    input::Device input;
    u8 buttons = 0;
};

void keyboard_report(void* ctx, const u8* data, u32 len) {
    auto* k = static_cast<Keyboard*>(ctx);
    if (len < 8 || data[2] == 1)
        return; // short, or "too many keys pressed" (phantom state)
    // Modifiers: bits 0-7 of byte 0 are HID usages 0xE0-0xE7.
    u8 changed = data[0] ^ k->last[0];
    for (int i = 0; i < 8; i++) {
        if (!(changed & (1 << i)))
            continue;
        bool down = data[0] & (1 << i);
        u16 code = keyboard_usage_to_key(static_cast<u8>(0xE0 + i));
        k->input.report(input::EV_KEY, code, down);
        ps2kbd::console_key(code, down);
    }
    // Keys: up to six usages; compare with the previous report.
    auto contains = [](const u8* r, u8 usage) {
        for (int i = 2; i < 8; i++)
            if (r[i] == usage)
                return true;
        return false;
    };
    for (int i = 2; i < 8; i++) {
        u8 u = k->last[i];
        if (u > 3 && !contains(data, u) && keyboard_usage_to_key(u)) {
            k->input.report(input::EV_KEY, keyboard_usage_to_key(u), 0);
            ps2kbd::console_key(keyboard_usage_to_key(u), false);
        }
    }
    for (int i = 2; i < 8; i++) {
        u8 u = data[i];
        if (u > 3 && !contains(k->last, u) && keyboard_usage_to_key(u)) {
            k->input.report(input::EV_KEY, keyboard_usage_to_key(u), 1);
            ps2kbd::console_key(keyboard_usage_to_key(u), true);
        }
    }
    k->input.sync();
    memcpy(k->last, data, 8);
}

void mouse_report(void* ctx, const u8* data, u32 len) {
    auto* m = static_cast<Mouse*>(ctx);
    if (len < 3)
        return;
    i32 dx = static_cast<i8>(data[1]), dy = static_cast<i8>(data[2]);
    i32 wheel = len >= 4 ? static_cast<i8>(data[3]) : 0;
    if (dx) m->input.report(input::EV_REL, input::REL_X, dx);
    if (dy) m->input.report(input::EV_REL, input::REL_Y, dy); // USB: down is positive, like evdev
    if (wheel) m->input.report(input::EV_REL, input::REL_WHEEL, wheel);
    static const u16 codes[5] = {input::BTN_LEFT, input::BTN_RIGHT, input::BTN_MIDDLE, input::BTN_SIDE,
                                 input::BTN_EXTRA};
    u8 buttons = data[0] & 0x1F;
    for (int i = 0; i < 5; i++)
        if ((buttons ^ m->buttons) & (1 << i))
            m->input.report(input::EV_KEY, codes[i], (buttons >> i) & 1);
    m->buttons = buttons;
    m->input.sync();
}

void boot_disconnect(void* ctx) {
    // Keyboard and Mouse both start with their input::Device.
    auto* in = static_cast<input::Device*>(ctx);
    input::unregister_device(in);
    // Never freed: they're tiny, and which of the two types it is isn't known here.
}

bool probe_boot(Device* d, const Interface& intf, const Endpoint* ep) {
    d->control(0x21, REQ_SET_PROTOCOL, PROTOCOL_BOOT, intf.number, nullptr, 0);
    if (intf.protocol == 1) {
        auto* k = new Keyboard;
        name_device(k->input, d, "");
        k->input.ev_bits = (1ull << input::EV_KEY) | (1ull << input::EV_REP);
        for (u32 u = 0; u < 256; u++)
            if (keyboard_usage_to_key(static_cast<u8>(u)))
                k->input.set_key(keyboard_usage_to_key(static_cast<u8>(u)));
        input::register_device(&k->input);
        d->add_binding(boot_disconnect, &k->input);
        d->start_interrupt_in(ep->address, 8, keyboard_report, k);
    } else {
        auto* m = new Mouse;
        name_device(m->input, d, "");
        m->input.ev_bits = (1ull << input::EV_KEY) | (1ull << input::EV_REL);
        m->input.rel_bits = (1ull << input::REL_X) | (1ull << input::REL_Y) | (1ull << input::REL_WHEEL);
        for (u16 b = input::BTN_LEFT; b <= input::BTN_EXTRA; b++)
            m->input.set_key(b);
        input::register_device(&m->input);
        d->add_binding(boot_disconnect, &m->input);
        d->start_interrupt_in(ep->address, ep->max_packet < 8 ? ep->max_packet : 8, mouse_report, m);
    }
    return true;
}

} // namespace

bool probe(Device* d, const Interface& intf) {
    if (intf.cls != 3)
        return false;
    const Endpoint* ep = nullptr;
    for (int i = 0; i < intf.endpoint_count; i++)
        if (intf.endpoints[i].type() == 3 && intf.endpoints[i].is_in())
            ep = &intf.endpoints[i];
    if (!ep)
        return false;
    bool boot_capable = intf.subclass == 1 && (intf.protocol == 1 || intf.protocol == 2);

    // Report descriptor (requested from the interface, as its HID descriptor says).
    u16 desc_len = intf.hid_report_length ? intf.hid_report_length : 512;
    if (desc_len > 4096)
        desc_len = 4096;
    auto* raw = static_cast<u8*>(kmalloc(desc_len));
    long n = raw ? d->control(0x81, REQ_GET_DESCRIPTOR, DESC_HID_REPORT << 8, intf.number, raw, desc_len) : -1;

    auto* r = new ReportDevice;
    int usable = n > 0 ? r->hid.init(raw, static_cast<usize>(n), r) : 0;
    kfree(raw);
    if (!usable) {
        delete r;
        if (!boot_capable) {
            klog::warn("usb-hid: %s: report descriptor not usable", d->product_name);
            return false;
        }
        d->control(0x21, REQ_SET_IDLE, 0, intf.number, nullptr, 0);
        if (d->open_endpoint(*ep) != 0)
            return false;
        return probe_boot(d, intf, ep);
    }

    if (intf.subclass == 1)
        d->control(0x21, REQ_SET_PROTOCOL, PROTOCOL_REPORT, intf.number, nullptr, 0); // boot devices: be explicit
    d->control(0x21, REQ_SET_IDLE, 0, intf.number, nullptr, 0); // report only on change
    if (d->open_endpoint(*ep) != 0) {
        delete r;
        return false;
    }
    HidInput& hid = r->hid;
    int named = 0;
    for (int i = 0; i < hid.device_count; i++) {
        if (!hid.device_used[i])
            continue;
        name_device(hid.devices[i], d, suffix_for(hid.desc.applications[i], usable));
        input::register_device(&hid.devices[i]);
        named++;
    }
    d->add_binding(report_disconnect, r);
    u32 length = hid.max_report_bytes();
    if (length < ep->max_packet)
        length = ep->max_packet; // a whole packet, or the controller reports babble
    d->start_interrupt_in(ep->address, length, report_received, r);
    klog::info("usb-hid: %s: %d input device(s), %d report fields%s", d->product_name, named, hid.desc.field_count,
               hid.desc.uses_report_ids ? ", report IDs" : "");
    return true;
}

} // namespace usb::hid
