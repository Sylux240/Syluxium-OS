// USB HID class driver: boot-protocol keyboards and mice.
//
// Both feed /dev/input/eventN like the PS/2 driver; keyboards also feed
// the console. Full report-descriptor parsing (gamepads, tablets, media
// keys) builds on this later.

#include <core/log.hpp>
#include <drivers/input/input.hpp>
#include <drivers/ps2kbd.hpp>
#include <drivers/usb/usb.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>

namespace usb::hid {

namespace {

// HID usage (keyboard page) -> Linux key code; same table as Linux's usbkbd.
constexpr u8 keymap[256] = {
    0,   0,   0,   0,   30,  48,  46,  32,  18,  33,  34,  35,  23,  36,  37,  38,
    50,  49,  24,  25,  16,  19,  31,  20,  22,  47,  17,  45,  21,  44,  2,   3,
    4,   5,   6,   7,   8,   9,   10,  11,  28,  1,   14,  15,  57,  12,  13,  26,
    27,  43,  43,  39,  40,  41,  51,  52,  53,  58,  59,  60,  61,  62,  63,  64,
    65,  66,  67,  68,  87,  88,  99,  70,  119, 110, 102, 104, 111, 107, 109, 106,
    105, 108, 103, 69,  98,  55,  74,  78,  96,  79,  80,  81,  75,  76,  77,  71,
    72,  73,  82,  83,  86,  127, 116, 117, 183, 184, 185, 186, 187, 188, 189, 190,
    191, 192, 193, 194, 134, 138, 130, 132, 128, 129, 131, 137, 133, 135, 136, 113,
    115, 114, 0,   0,   0,   121, 0,   89,  93,  124, 92,  94,  95,  0,   0,   0,
    122, 123, 90,  91,  85,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    29,  42,  56,  125, 97,  54,  100, 126, 164, 166, 165, 163, 161, 115, 114, 113,
    150, 158, 159, 128, 136, 177, 178, 176, 142, 152, 173, 140,
};

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
        u16 code = keymap[0xE0 + i];
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
        if (u > 3 && !contains(data, u) && keymap[u]) {
            k->input.report(input::EV_KEY, keymap[u], 0);
            ps2kbd::console_key(keymap[u], false);
        }
    }
    for (int i = 2; i < 8; i++) {
        u8 u = data[i];
        if (u > 3 && !contains(k->last, u) && keymap[u]) {
            k->input.report(input::EV_KEY, keymap[u], 1);
            ps2kbd::console_key(keymap[u], true);
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

void name_device(input::Device& in, Device* d) {
    snprintf(in.name, sizeof(in.name), "%s", d->product_name);
    in.bustype = input::BUS_USB;
    in.vendor = d->vendor;
    in.product = d->product;
}

} // namespace

bool probe(Device* d, const Interface& intf) {
    // Boot-interface keyboards (protocol 1) and mice (protocol 2).
    if (intf.cls != 3 || intf.subclass != 1 || (intf.protocol != 1 && intf.protocol != 2))
        return false;
    const Endpoint* ep = nullptr;
    for (int i = 0; i < intf.endpoint_count; i++)
        if (intf.endpoints[i].type() == 3 && intf.endpoints[i].is_in())
            ep = &intf.endpoints[i];
    if (!ep)
        return false;

    d->control(0x21, 0x0B, 0, intf.number, nullptr, 0); // SET_PROTOCOL: boot
    d->control(0x21, 0x0A, 0, intf.number, nullptr, 0); // SET_IDLE: report only on change
    if (d->open_endpoint(*ep) != 0)
        return false;

    if (intf.protocol == 1) {
        auto* k = new Keyboard;
        name_device(k->input, d);
        k->input.ev_bits = (1ull << input::EV_KEY) | (1ull << input::EV_REP);
        for (u32 u = 0; u < 256; u++)
            if (keymap[u])
                k->input.set_key(keymap[u]);
        input::register_device(&k->input);
        d->start_interrupt_in(ep->address, 8, keyboard_report, k);
    } else {
        auto* m = new Mouse;
        name_device(m->input, d);
        m->input.ev_bits = (1ull << input::EV_KEY) | (1ull << input::EV_REL);
        m->input.rel_bits = (1ull << input::REL_X) | (1ull << input::REL_Y) | (1ull << input::REL_WHEEL);
        for (u16 b = input::BTN_LEFT; b <= input::BTN_EXTRA; b++)
            m->input.set_key(b);
        input::register_device(&m->input);
        d->start_interrupt_in(ep->address, ep->max_packet < 8 ? ep->max_packet : 8, mouse_report, m);
    }
    return true;
}

} // namespace usb::hid
