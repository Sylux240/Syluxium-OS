#pragma once

#include <drivers/input/touchpad.hpp>
#include <drivers/usb/hid_parser.hpp>
#include <drivers/usb/hid_touchpad.hpp>

// One HID device on any transport (USB, I2C): its report descriptor turned
// into input devices (HidInput), keyboards also feeding the text console,
// and a precision touchpad's fingers turned into pointer events.
namespace usb::hid {

class HidDevice final : public EventSink {
public:
    // Parse the descriptor. Returns the number of usable input devices (0: none).
    int parse(const u8* descriptor, usize length);
    // Name and register the input devices; a touchpad is switched to
    // multi-touch reporting through `io` (may be null: stays a mouse).
    // Returns the number of devices registered.
    int register_devices(const char* product_name, const char* tag, u16 bustype, u16 vendor, u16 product,
                         FeatureIo* io);
    // One input report as received (with the report-ID byte if used).
    void report(const u8* data, u32 length);
    // Unplugged: remove the input devices (no report() may follow).
    void unregister_devices();

    void event(int app, u16 type, u16 code, i32 value) override;
    u32 max_report_bytes() const { return hid.max_report_bytes(); }

    HidInput hid;
    Touchpad touchpad;
    input::TouchpadMouse* pad = nullptr; // set when the touchpad is in multi-touch mode
    char name[48] = {};                  // for log messages
    u8 logged = 0;
};

} // namespace usb::hid
