// Xbox 360 and Xbox One controllers (and the many pads that copy them).
//
// They aren't HID devices: they use a vendor-specific interface (class 0xFF)
// with fixed report formats, which Linux's xpad driver decodes. We produce
// the same events as xpad - ABS_X/Y/RX/RY sticks (Y axes pointing up),
// ABS_Z/RZ triggers, the D-pad as ABS_HAT0X/Y, BTN_A/B/X/Y (= SOUTH/EAST/
// NORTH/WEST), shoulders, start/back/guide, stick clicks - so SDL and Steam
// recognize them as the standard gamepad they are.
//
//  - Xbox 360 (wired): subclass 0x5D, protocol 0x01; 20-byte input reports.
//  - Xbox One: subclass 0x47, protocol 0xD0 ("GIP"); must be told to power
//    on, then sends type 0x20 reports (buttons, triggers, sticks) and type
//    0x07 reports for the guide button.

#include <core/log.hpp>
#include <core/panic.hpp>
#include <drivers/input/input.hpp>
#include <drivers/usb/hid_parser.hpp>
#include <drivers/usb/usb.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>

namespace usb::xpad {

namespace {

enum class Kind : u8 { Xbox360, XboxOne };

i16 le16(const u8* p) { return static_cast<i16>(p[0] | (p[1] << 8)); }

struct Sink : hid::EventSink {
    input::Device* input = nullptr;
    void event(int, u16 type, u16 code, i32 value) override { input->report(type, code, value); }
};

struct Pad {
    input::Device input;
    Sink sink;
    Kind kind;
    Device* usb = nullptr;
    u8 out_ep = 0;
};

void key(hid::EventSink* s, u16 code, bool down) { s->event(0, input::EV_KEY, code, down ? 1 : 0); }
void abs(hid::EventSink* s, u16 code, i32 value) { s->event(0, input::EV_ABS, code, value); }

// Xbox 360: byte 0 = message type (0x00 = input), byte 1 = length (0x14).
void decode_360(const u8* d, u32 len, hid::EventSink* s) {
    if (len < 14 || d[0] != 0x00 || d[1] < 0x14)
        return; // LED status / headset messages etc.
    abs(s, input::ABS_HAT0X, !!(d[2] & 0x08) - !!(d[2] & 0x04));
    abs(s, input::ABS_HAT0Y, !!(d[2] & 0x02) - !!(d[2] & 0x01));
    key(s, input::BTN_START, d[2] & 0x10);
    key(s, input::BTN_SELECT, d[2] & 0x20);
    key(s, input::BTN_THUMBL, d[2] & 0x40);
    key(s, input::BTN_THUMBR, d[2] & 0x80);
    key(s, input::BTN_TL, d[3] & 0x01);
    key(s, input::BTN_TR, d[3] & 0x02);
    key(s, input::BTN_MODE, d[3] & 0x04);
    key(s, input::BTN_SOUTH, d[3] & 0x10); // A
    key(s, input::BTN_EAST, d[3] & 0x20);  // B
    key(s, input::BTN_NORTH, d[3] & 0x40); // X (Linux's BTN_X)
    key(s, input::BTN_WEST, d[3] & 0x80);  // Y (Linux's BTN_Y)
    abs(s, input::ABS_Z, d[4]);
    abs(s, input::ABS_RZ, d[5]);
    abs(s, input::ABS_X, le16(d + 6));
    abs(s, input::ABS_Y, ~le16(d + 8)); // up is positive on the wire, negative in evdev
    abs(s, input::ABS_RX, le16(d + 10));
    abs(s, input::ABS_RY, ~le16(d + 12));
    s->event(0, input::EV_SYN, input::SYN_REPORT, 0);
}

// Xbox One (GIP): byte 0 = command, byte 1 = flags, byte 2 = sequence, byte 3 = length.
void decode_one(const u8* d, u32 len, hid::EventSink* s) {
    if (len >= 5 && d[0] == 0x07) { // guide button
        key(s, input::BTN_MODE, d[4] & 0x01);
        s->event(0, input::EV_SYN, input::SYN_REPORT, 0);
        return;
    }
    if (len < 18 || d[0] != 0x20)
        return;
    key(s, input::BTN_START, d[4] & 0x04);  // menu
    key(s, input::BTN_SELECT, d[4] & 0x08); // view
    key(s, input::BTN_SOUTH, d[4] & 0x10);
    key(s, input::BTN_EAST, d[4] & 0x20);
    key(s, input::BTN_NORTH, d[4] & 0x40);
    key(s, input::BTN_WEST, d[4] & 0x80);
    abs(s, input::ABS_HAT0X, !!(d[5] & 0x08) - !!(d[5] & 0x04));
    abs(s, input::ABS_HAT0Y, !!(d[5] & 0x02) - !!(d[5] & 0x01));
    key(s, input::BTN_TL, d[5] & 0x10);
    key(s, input::BTN_TR, d[5] & 0x20);
    key(s, input::BTN_THUMBL, d[5] & 0x40);
    key(s, input::BTN_THUMBR, d[5] & 0x80);
    abs(s, input::ABS_Z, static_cast<u16>(le16(d + 6)));
    abs(s, input::ABS_RZ, static_cast<u16>(le16(d + 8)));
    abs(s, input::ABS_X, le16(d + 10));
    abs(s, input::ABS_Y, ~le16(d + 12));
    abs(s, input::ABS_RX, le16(d + 14));
    abs(s, input::ABS_RY, ~le16(d + 16));
    s->event(0, input::EV_SYN, input::SYN_REPORT, 0);
}

void received(void* ctx, const u8* data, u32 length) {
    auto* p = static_cast<Pad*>(ctx);
    if (p->kind == Kind::Xbox360)
        decode_360(data, length, &p->sink);
    else
        decode_one(data, length, &p->sink);
}

void disconnect(void* ctx) {
    auto* p = static_cast<Pad*>(ctx);
    input::unregister_device(&p->input);
    delete p;
}

void declare(input::Device& in, Kind kind) {
    in.ev_bits = (1ull << input::EV_KEY) | (1ull << input::EV_ABS);
    static constexpr u16 BUTTONS[] = {input::BTN_SOUTH, input::BTN_EAST, input::BTN_NORTH, input::BTN_WEST,
                                      input::BTN_TL,    input::BTN_TR,   input::BTN_SELECT, input::BTN_START,
                                      input::BTN_MODE,  input::BTN_THUMBL, input::BTN_THUMBR};
    for (u16 b : BUTTONS)
        in.set_key(b);
    static constexpr u16 STICKS[] = {input::ABS_X, input::ABS_Y, input::ABS_RX, input::ABS_RY};
    for (u16 axis : STICKS)
        in.set_abs(axis, -32768, 32767, 128);
    i32 trigger_max = kind == Kind::Xbox360 ? 255 : 1023;
    in.set_abs(input::ABS_Z, 0, trigger_max);
    in.set_abs(input::ABS_RZ, 0, trigger_max);
    in.set_abs(input::ABS_HAT0X, -1, 1);
    in.set_abs(input::ABS_HAT0Y, -1, 1);
}

} // namespace

bool probe(Device* d, const Interface& intf) {
    Kind kind;
    if (intf.cls == 0xFF && intf.subclass == 0x5D && intf.protocol == 0x01)
        kind = Kind::Xbox360;
    else if (intf.cls == 0xFF && intf.subclass == 0x47 && intf.protocol == 0xD0)
        kind = Kind::XboxOne;
    else
        return false;
    const Endpoint* in = nullptr;
    const Endpoint* out = nullptr;
    for (int i = 0; i < intf.endpoint_count; i++) {
        const Endpoint& e = intf.endpoints[i];
        if (e.type() == 3)
            (e.is_in() ? in : out) = &e;
    }
    if (!in || d->open_endpoint(*in) != 0)
        return false;

    auto* p = new Pad;
    p->kind = kind;
    p->usb = d;
    p->sink.input = &p->input;
    snprintf(p->input.name, sizeof(p->input.name), "%s",
             kind == Kind::Xbox360 ? "Microsoft X-Box 360 pad" : "Microsoft X-Box One pad");
    p->input.bustype = input::BUS_USB;
    p->input.vendor = d->vendor;
    p->input.product = d->product;
    declare(p->input, kind);

    if (out && d->open_endpoint(*out) == 0) {
        p->out_ep = out->address;
        if (kind == Kind::Xbox360) {
            u8 led[3] = {0x01, 0x03, 0x06}; // ring LED: player 1
            d->transfer(p->out_ep, led, sizeof(led));
        } else {
            u8 power_on[5] = {0x05, 0x20, 0x00, 0x01, 0x00}; // GIP: start sending input
            d->transfer(p->out_ep, power_on, sizeof(power_on));
        }
    }
    input::register_device(&p->input);
    d->add_binding(disconnect, p);
    d->start_interrupt_in(in->address, in->max_packet ? in->max_packet : 64, received, p);
    klog::info("xpad: %s (%04x:%04x)", p->input.name, d->vendor, d->product);
    return true;
}

// ---- self-test: decode synthetic packets ----

namespace {

struct Recorder : hid::EventSink {
    struct Ev {
        u16 type, code;
        i32 value;
    };
    Ev events[64];
    int count = 0;
    void event(int, u16 type, u16 code, i32 value) override {
        if (count < 64)
            events[count++] = Ev{type, code, value};
    }
    bool has(u16 type, u16 code, i32 value) const {
        for (int i = 0; i < count; i++)
            if (events[i].type == type && events[i].code == code && events[i].value == value)
                return true;
        return false;
    }
};

} // namespace

void run_selftest() {
    // Xbox 360: A + LB + D-pad right, left stick full right / full up, RT half.
    const u8 pkt360[20] = {0x00, 0x14, 0x08, 0x11, 0x00, 0x80, 0xFF, 0x7F, 0xFF, 0x7F, 0, 0, 0, 0};
    Recorder r;
    decode_360(pkt360, sizeof(pkt360), &r);
    bool ok360 = r.has(input::EV_KEY, input::BTN_SOUTH, 1) && r.has(input::EV_KEY, input::BTN_TL, 1) &&
                 r.has(input::EV_KEY, input::BTN_EAST, 0) && r.has(input::EV_ABS, input::ABS_HAT0X, 1) &&
                 r.has(input::EV_ABS, input::ABS_X, 32767) && r.has(input::EV_ABS, input::ABS_Y, -32768) &&
                 r.has(input::EV_ABS, input::ABS_RZ, 0x80);
    // Xbox One: Y + view, LT max, right stick full down.
    const u8 pkt1[18] = {0x20, 0x00, 0x01, 0x0E, 0x88, 0x00, 0xFF, 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x80};
    Recorder r1;
    decode_one(pkt1, sizeof(pkt1), &r1);
    bool ok1 = r1.has(input::EV_KEY, input::BTN_WEST, 1) && r1.has(input::EV_KEY, input::BTN_SELECT, 1) &&
               r1.has(input::EV_ABS, input::ABS_Z, 1023) && r1.has(input::EV_ABS, input::ABS_RY, 32767);
    if (!ok360 || !ok1)
        panic("xpad self-test failed (360: %d, One: %d)", ok360, ok1);
    klog::ok("xpad self-test: Xbox 360 and Xbox One reports decode to the standard gamepad events");
}

} // namespace usb::xpad
