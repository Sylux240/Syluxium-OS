// Boot-time self-test of the HID report parser and the input mapping, using
// report descriptors of real devices (QEMU can't emulate gamepads): each
// descriptor is parsed, then sample reports are fed in and the resulting
// evdev events and capabilities are checked against what Linux produces.

#include <core/log.hpp>
#include <core/panic.hpp>
#include <drivers/usb/hid_parser.hpp>
#include <drivers/usb/usb.hpp>
#include <mm/heap.hpp>

namespace usb::hid {

namespace {

// QEMU usb-tablet (hw/usb/dev-hid.c): absolute X/Y 0..32767, 3 buttons, wheel.
constexpr u8 QEMU_TABLET[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x7f, 0x35, 0x00, 0x46, 0xff, 0x7f,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x35, 0x00,
    0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xc0, 0xc0,
};

// DragonRise "Generic USB Joystick" (0079:0006), sold as countless cheap
// gamepads: joystick application, 5 axes, a 4-bit hat with null state,
// 12 buttons, vendor-page padding and an output report.
constexpr u8 DRAGONRISE[] = {
    0x05, 0x01, 0x09, 0x04, 0xA1, 0x01, 0xA1, 0x02, 0x75, 0x08, 0x95, 0x05, 0x15, 0x00, 0x26, 0xFF,
    0x00, 0x35, 0x00, 0x46, 0xFF, 0x00, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x32, 0x09, 0x35,
    0x81, 0x02, 0x75, 0x04, 0x95, 0x01, 0x25, 0x07, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x09, 0x39, 0x81,
    0x42, 0x65, 0x00, 0x75, 0x01, 0x95, 0x0C, 0x25, 0x01, 0x45, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29,
    0x0C, 0x81, 0x02, 0x06, 0x00, 0xFF, 0x75, 0x01, 0x95, 0x08, 0x25, 0x01, 0x45, 0x01, 0x09, 0x01,
    0x81, 0x02, 0xC0, 0xA1, 0x02, 0x75, 0x08, 0x95, 0x04, 0x46, 0xFF, 0x00, 0x26, 0xFF, 0x00, 0x09,
    0x02, 0x91, 0x02, 0xC0, 0xC0,
};

// A typical modern gamepad (DirectInput mode of 8BitDo/Logitech-style pads):
// gamepad application with report ID 1, 16 buttons, hat with null state,
// signed sticks X/Y/Z/Rz and unsigned analog triggers Rx/Ry.
constexpr u8 GAMEPAD[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75,
    0x04, 0x95, 0x01, 0x81, 0x42, 0x65, 0x00, 0x75, 0x04, 0x95, 0x01, 0x81, 0x03,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
    0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
    0xC0,
};

// Keyboard + consumer control in one interface, told apart by report IDs
// (how most keyboards with media keys do it).
constexpr u8 KEYBOARD_MEDIA[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02, 0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A,
    0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00, 0xC0,
};

// 5-button mouse with 12-bit relative X/Y, wheel and horizontal scroll (AC Pan).
constexpr u8 MOUSE5[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x05,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
    0x05, 0x01, 0x16, 0x01, 0xF8, 0x26, 0xFF, 0x07, 0x75, 0x0C, 0x95, 0x02, 0x09, 0x30, 0x09, 0x31,
    0x81, 0x06, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x09, 0x38, 0x81, 0x06,
    0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0,
};

struct Recorder : EventSink {
    struct Ev {
        int app;
        u16 type, code;
        i32 value;
    };
    Ev events[128];
    int count = 0;
    void event(int app, u16 type, u16 code, i32 value) override {
        if (count < 128)
            events[count++] = Ev{app, type, code, value};
    }
    bool has(int app, u16 type, u16 code, i32 value) const {
        for (int i = 0; i < count; i++)
            if (events[i].app == app && events[i].type == type && events[i].code == code && events[i].value == value)
                return true;
        return false;
    }
    void clear() { count = 0; }
};

struct Case {
    HidInput hid;
    Recorder rec;
};

int g_failures = 0;

void expect(bool ok, const char* device, const char* what) {
    if (!ok) {
        klog::error("hid self-test: %s: %s", device, what);
        g_failures++;
    }
}

Case* load(const char* name, const u8* desc, usize len, int expected_devices) {
    auto* c = new Case;
    int used = c->hid.init(desc, len, &c->rec);
    expect(used == expected_devices, name, "wrong number of input devices");
    return c;
}

} // namespace

void run_selftest() {
    using namespace input;
    g_failures = 0;

    { // QEMU tablet: absolute pointer.
        Case* c = load("QEMU tablet", QEMU_TABLET, sizeof(QEMU_TABLET), 1);
        input::Device& d = c->hid.devices[0];
        expect(d.has_key(BTN_LEFT) && d.has_key(BTN_MIDDLE) && (d.abs_bits & 3) == 3 && d.abs_max[ABS_X] == 32767 &&
                   (d.rel_bits & (1ull << REL_WHEEL)),
               "QEMU tablet", "capabilities");
        const u8 r[] = {0x01, 0x00, 0x40, 0x34, 0x12, 0xFF};
        c->hid.process(r, sizeof(r));
        expect(c->rec.has(0, EV_KEY, BTN_LEFT, 1) && c->rec.has(0, EV_ABS, ABS_X, 0x4000) &&
                   c->rec.has(0, EV_ABS, ABS_Y, 0x1234) && c->rec.has(0, EV_REL, REL_WHEEL, -1) &&
                   c->rec.has(0, EV_REL, REL_WHEEL_HI_RES, -120) && c->rec.has(0, EV_SYN, SYN_REPORT, 0),
               "QEMU tablet", "report events");
        delete c;
    }
    { // DragonRise joystick.
        Case* c = load("DragonRise joystick", DRAGONRISE, sizeof(DRAGONRISE), 1);
        input::Device& d = c->hid.devices[0];
        expect(d.has_key(BTN_JOYSTICK) && d.has_key(BTN_JOYSTICK + 11) && !d.has_key(BTN_JOYSTICK + 12) &&
                   (d.abs_bits & (1ull << ABS_X)) && (d.abs_bits & (1ull << ABS_RZ)) &&
                   (d.abs_bits & (1ull << ABS_HAT0X)) && d.abs_max[ABS_X] == 255,
               "DragonRise joystick", "capabilities");
        // X=16 (left), Y=128, hat 6 (west), buttons 1 and 12.
        const u8 r[] = {0x10, 0x80, 0x80, 0x80, 0x80, 0x16, 0x80, 0x00};
        c->hid.process(r, sizeof(r));
        expect(c->rec.has(0, EV_ABS, ABS_X, 16) && c->rec.has(0, EV_ABS, ABS_HAT0X, -1) &&
                   c->rec.has(0, EV_ABS, ABS_HAT0Y, 0) && c->rec.has(0, EV_KEY, BTN_JOYSTICK, 1) &&
                   c->rec.has(0, EV_KEY, BTN_JOYSTICK + 11, 1),
               "DragonRise joystick", "report events");
        delete c;
    }
    { // Gamepad with report ID.
        Case* c = load("gamepad", GAMEPAD, sizeof(GAMEPAD), 1);
        input::Device& d = c->hid.devices[0];
        expect(d.has_key(BTN_SOUTH) && d.has_key(BTN_THUMBR) && d.has_key(BTN_GAMEPAD + 15) &&
                   d.abs_min[ABS_X] == -127 && d.abs_max[ABS_RX] == 255 && d.abs_flat[ABS_X] > 0 &&
                   (d.abs_bits & (1ull << ABS_HAT0Y)),
               "gamepad", "capabilities (BTN_SOUTH.., signed sticks, triggers, hat)");
        // Button 1 (A) + 16, hat east, X right, Y up, Rx (left trigger) full.
        const u8 r[] = {0x01, 0x01, 0x80, 0x02, 0x7F, 0x81, 0x00, 0x00, 0xFF, 0x00};
        c->hid.process(r, sizeof(r));
        expect(c->rec.has(0, EV_KEY, BTN_SOUTH, 1) && c->rec.has(0, EV_KEY, BTN_GAMEPAD + 15, 1) &&
                   c->rec.has(0, EV_ABS, ABS_HAT0X, 1) && c->rec.has(0, EV_ABS, ABS_X, 127) &&
                   c->rec.has(0, EV_ABS, ABS_Y, -127) && c->rec.has(0, EV_ABS, ABS_RX, 255),
               "gamepad", "press report");
        c->rec.clear();
        const u8 r2[] = {0x01, 0x00, 0x00, 0x08, 0x7F, 0x81, 0x00, 0x00, 0xFF, 0x00}; // hat null state
        c->hid.process(r2, sizeof(r2));
        expect(c->rec.has(0, EV_KEY, BTN_SOUTH, 0) && c->rec.has(0, EV_ABS, ABS_HAT0X, 0) &&
                   !c->rec.has(0, EV_ABS, ABS_X, 127), // unchanged axes aren't repeated
               "gamepad", "release report");
        c->rec.clear();
        const u8 other[] = {0x02, 0xFF, 0xFF}; // a report ID the descriptor doesn't have
        c->hid.process(other, sizeof(other));
        expect(c->rec.count == 0, "gamepad", "unknown report ID ignored");
        delete c;
    }
    { // Keyboard + media keys: two devices.
        Case* c = load("keyboard + media", KEYBOARD_MEDIA, sizeof(KEYBOARD_MEDIA), 2);
        expect(c->hid.is_keyboard[0] && c->hid.devices[0].has_key(30) && c->hid.devices[1].has_key(KEY_VOLUMEUP) &&
                   c->hid.devices[1].has_key(KEY_PLAYPAUSE),
               "keyboard + media", "capabilities");
        const u8 keys[] = {0x01, 0x02, 0x00, 0x04, 0, 0, 0, 0, 0}; // left shift + 'a'
        c->hid.process(keys, sizeof(keys));
        expect(c->rec.has(0, EV_KEY, 42, 1) && c->rec.has(0, EV_KEY, 30, 1), "keyboard + media", "key press");
        c->rec.clear();
        const u8 up[] = {0x01, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0};
        c->hid.process(up, sizeof(up));
        expect(c->rec.has(0, EV_KEY, 42, 0) && c->rec.has(0, EV_KEY, 30, 0), "keyboard + media", "key release");
        c->rec.clear();
        const u8 rollover[] = {0x01, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01}; // ErrorRollOver
        c->hid.process(rollover, sizeof(rollover));
        expect(c->rec.count == 0, "keyboard + media", "phantom state ignored");
        const u8 vol[] = {0x02, 0xE9, 0x00};
        c->hid.process(vol, sizeof(vol));
        expect(c->rec.has(1, EV_KEY, KEY_VOLUMEUP, 1), "keyboard + media", "volume up on the consumer device");
        c->rec.clear();
        const u8 vol_up[] = {0x02, 0x00, 0x00};
        c->hid.process(vol_up, sizeof(vol_up));
        expect(c->rec.has(1, EV_KEY, KEY_VOLUMEUP, 0), "keyboard + media", "volume up released");
        delete c;
    }
    { // 5-button mouse with 12-bit axes and horizontal scrolling.
        Case* c = load("5-button mouse", MOUSE5, sizeof(MOUSE5), 1);
        input::Device& d = c->hid.devices[0];
        expect(d.has_key(BTN_EXTRA) && (d.rel_bits & (1ull << REL_HWHEEL)) &&
                   (d.rel_bits & (1ull << REL_WHEEL_HI_RES)) && !(d.ev_bits & (1ull << EV_ABS)),
               "5-button mouse", "capabilities");
        const u8 r[] = {0x10, 0x05, 0xD0, 0xFF, 0x01, 0xFF}; // button 5, dx +5, dy -3, wheel +1, pan -1
        c->hid.process(r, sizeof(r));
        expect(c->rec.has(0, EV_KEY, BTN_EXTRA, 1) && c->rec.has(0, EV_REL, REL_X, 5) &&
                   c->rec.has(0, EV_REL, REL_Y, -3) && c->rec.has(0, EV_REL, REL_WHEEL, 1) &&
                   c->rec.has(0, EV_REL, REL_HWHEEL, -1) && c->rec.has(0, EV_REL, REL_HWHEEL_HI_RES, -120),
               "5-button mouse", "report events");
        delete c;
    }
    { // Malformed descriptors must be rejected, not crash.
        const u8 unbalanced[] = {0x05, 0x01, 0x09, 0x02, 0xA1, 0x01};
        const u8 truncated[] = {0x05, 0x01, 0x26, 0xFF};
        auto* rd = static_cast<ReportDescriptor*>(kmalloc(sizeof(ReportDescriptor)));
        expect(parse(unbalanced, sizeof(unbalanced), rd) != 0 && parse(truncated, sizeof(truncated), rd) != 0,
               "malformed", "rejected");
        kfree(rd);
    }

    if (g_failures)
        panic("HID self-test: %d check(s) failed", g_failures);
    klog::ok("HID self-test: tablet, joystick, gamepad, keyboard + media keys, 5-button mouse map like Linux");
}

} // namespace usb::hid
