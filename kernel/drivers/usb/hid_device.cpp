// A HID device on any transport (see hid_device.hpp).

#include <core/log.hpp>
#include <drivers/ps2kbd.hpp>
#include <drivers/usb/hid_device.hpp>
#include <lib/format.hpp>

namespace usb::hid {

namespace {

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
    case DG_TOUCHPAD: return "Touchpad";
    default: return "";
    }
}

} // namespace

int HidDevice::parse(const u8* descriptor, usize length) { return hid.init(descriptor, length, this); }

void HidDevice::event(int app, u16 type, u16 code, i32 value) {
    hid.devices[app].report(type, code, value);
    // Keyboard keys also go to the text console (codes below BTN_MISC are keys).
    if (hid.is_keyboard[app] && type == input::EV_KEY && code < input::BTN_MISC)
        ps2kbd::console_key(code, value != 0);
}

int HidDevice::register_devices(const char* product_name, const char* tag, u16 bustype, u16 vendor, u16 product,
                                FeatureIo* io) {
    snprintf(name, sizeof(name), "%s%s%s", product_name, tag[0] ? " " : "", tag);
    int usable = 0;
    for (int i = 0; i < hid.device_count; i++)
        usable += hid.device_used[i];
    if (touchpad.init(hid.desc)) {
        // The raw digitizer device would only confuse: the fingers become
        // pointer events, or (if the mode switch fails) the device goes on
        // sending its mouse reports.
        for (int i = 0; i < hid.device_count; i++)
            if (hid.desc.applications[i] == DG_TOUCHPAD)
                hid.device_used[i] = false;
        bool ok = io && touchpad.configure(*io);
        if (ok) {
            pad = new input::TouchpadMouse;
            char pname[80];
            snprintf(pname, sizeof(pname), "%s Touchpad", product_name);
            pad->init(pname, bustype, vendor, product, touchpad.units_per_mm);
            pad->set_area(touchpad.x_min, touchpad.x_max, touchpad.y_min, touchpad.y_max, touchpad.clickpad);
        }
        klog::info("hid-touchpad: %s: %d finger(s), x %d..%d, y %d..%d, %d units/mm%s: %s", name, touchpad.contacts,
                   touchpad.x_min, touchpad.x_max, touchpad.y_min, touchpad.y_max, touchpad.units_per_mm,
                   touchpad.clickpad ? ", clickpad" : "",
                   ok ? "multi-touch (pointer, two-finger scroll, tap)" : "mode switch failed, used as a mouse");
    }
    int registered = 0;
    for (int i = 0; i < hid.device_count; i++) {
        if (!hid.device_used[i])
            continue;
        input::Device& in = hid.devices[i];
        const char* suffix = suffix_for(hid.desc.applications[i], usable);
        snprintf(in.name, sizeof(in.name), "%s%s%s", product_name, suffix[0] ? " " : "", suffix);
        in.bustype = bustype;
        in.vendor = vendor;
        in.product = product;
        input::register_device(&in);
        registered++;
    }
    return registered + (pad ? 1 : 0);
}

void HidDevice::report(const u8* data, u32 length) {
    if (logged < 2) { // real-hardware diagnostics: do reports arrive, and what do they look like?
        logged++;
        char hex[3 * 16 + 1] = {};
        for (u32 i = 0; i < length && i < 16; i++)
            snprintf(hex + 3 * i, 4, "%02x ", data[i]);
        klog::info("hid: %s: report of %u bytes: %s", name, length, hex);
    }
    if (pad) {
        Touchpad::Frame f;
        if (touchpad.process(data, length, &f)) {
            pad->frame(f.fingers, f.count, f.button);
            return;
        }
        if (hid.desc.uses_report_ids && length && data[0] == touchpad.report_id())
            return; // part of a frame
    }
    hid.process(data, length);
}

void HidDevice::unregister_devices() {
    for (int i = 0; i < hid.device_count; i++)
        if (hid.device_used[i])
            input::unregister_device(&hid.devices[i]);
    if (pad)
        input::unregister_device(&pad->device());
}

} // namespace usb::hid
