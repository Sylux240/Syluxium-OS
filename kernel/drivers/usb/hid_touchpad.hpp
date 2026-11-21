#pragma once

#include <drivers/input/touchpad.hpp>
#include <drivers/usb/hid_parser.hpp>

// Windows "precision touchpads": HID digitizers with a Touch Pad application
// (usage 0x0D:0x05) reporting each finger (tip switch, confidence, contact
// ID, X, Y) plus a contact count and the pad's button. They come over USB
// and, on most laptops since ~2016, over I2C (i2c-hid). Until the host sets
// the "Input Mode" feature to 3 they act as a plain mouse; after that the
// fingers are ours and become pointer/scroll/tap events (TouchpadMouse).
// Report layouts as specified by Microsoft (and used by Linux's
// hid-multitouch); our own code.
namespace usb::hid {

// Feature-report access of the transport (USB control requests, I2C-HID
// commands). Buffers start with the report ID when the device uses IDs.
class FeatureIo {
public:
    virtual long get_feature(u8 id, u8* buf, u32 len) = 0;
    virtual long set_feature(u8 id, const u8* buf, u32 len) = 0;
};

class Touchpad {
public:
    static constexpr int MAX_CONTACTS = 10;

    struct Frame {
        input::TouchpadMouse::Finger fingers[MAX_CONTACTS]; // y growing upwards
        int count = 0;
        bool button = false;
    };

    // Finds the touch pad application and its finger collections.
    bool init(const ReportDescriptor& desc);
    // Switch the device to touchpad reporting (Input Mode = 3, surface and
    // button reporting on). False if it has no Input Mode feature or refused.
    bool configure(FeatureIo& io);
    // One input report (with its ID byte if the device uses IDs). True when
    // it completed a frame (`out` filled); reports of other applications
    // return false.
    bool process(const u8* report, u32 length, Frame* out);

    u8 report_id() const { return m_report_id; }
    i32 x_min = 0, x_max = 0, y_min = 0, y_max = 0;
    i32 units_per_mm = 0;
    int contacts = 0;      // finger collections in one report
    bool clickpad = true;  // the whole pad is the button (no separate buttons)

private:
    struct Loc {
        i16 field = -1;
        u16 elem = 0;
        bool valid() const { return field >= 0; }
    };
    struct Slot {
        u16 collection = 0;
        Loc tip, confidence, id, x, y;
    };
    i32 read(const Loc& l, const u8* data, u32 bytes) const;

    const ReportDescriptor* m_desc = nullptr;
    u8 m_report_id = 0;
    Slot m_slots[MAX_CONTACTS];
    Loc m_contact_count, m_button;
    Loc m_input_mode, m_surface_switch, m_button_switch, m_latency;
    // Frames split over several reports ("hybrid" mode).
    int m_expected = 0, m_received = 0;
    Frame m_frame;
};

// Write `size` bits of `value` at `bit_offset` (HID little-endian bit order).
void insert(u8* report, u32 report_bytes, u32 bit_offset, u8 size, u32 value);

} // namespace usb::hid
