#pragma once

#include <drivers/input/input.hpp>
#include <lib/types.hpp>

// HID report descriptors (HID 1.11, chapter 6.2.2) and their translation
// into Linux input events.
//
// A HID device describes its own reports: which bits mean what (usages such
// as "Generic Desktop X" or "Button 3"), their sizes, value ranges and
// whether they are absolute, relative or arrays of pressed keys. The parser
// turns that into a list of fields; HidInput then maps each usage to an evdev
// event the way Linux's hid-input does, so SDL, Steam, libinput and our
// desktop see the same codes as on Linux: mice (more buttons, wheels, tilt),
// absolute pointers and tablets, keyboards incl. consumer/media keys, and
// gamepads/joysticks (axes, hat switches, BTN_SOUTH...).
namespace usb::hid {

// Usages are 32 bits: usage page << 16 | usage id.
constexpr u32 usage(u16 page, u16 id) { return (static_cast<u32>(page) << 16) | id; }
constexpr u16 PAGE_GENERIC_DESKTOP = 0x01, PAGE_SIMULATION = 0x02, PAGE_KEYBOARD = 0x07, PAGE_LED = 0x08,
              PAGE_BUTTON = 0x09, PAGE_CONSUMER = 0x0C, PAGE_DIGITIZER = 0x0D;
constexpr u32 GD_POINTER = usage(1, 0x01), GD_MOUSE = usage(1, 0x02), GD_JOYSTICK = usage(1, 0x04),
              GD_GAMEPAD = usage(1, 0x05), GD_KEYBOARD = usage(1, 0x06), GD_KEYPAD = usage(1, 0x07),
              GD_SYSTEM_CONTROL = usage(1, 0x80), CONSUMER_CONTROL = usage(0x0C, 0x01),
              DG_DIGITIZER = usage(0x0D, 0x01), DG_PEN = usage(0x0D, 0x02), DG_TOUCHSCREEN = usage(0x0D, 0x04),
              DG_TOUCHPAD = usage(0x0D, 0x05), DG_DEVICE_CONFIG = usage(0x0D, 0x0E);

enum class ReportKind : u8 { Input, Output, Feature };

// Main item flag bits.
constexpr u32 FLAG_CONSTANT = 1 << 0, FLAG_VARIABLE = 1 << 1, FLAG_RELATIVE = 1 << 2, FLAG_NULL_STATE = 1 << 6;

struct Field {
    ReportKind kind;
    u8 report_id;        // 0 if the device doesn't use report IDs
    u8 size;             // bits per element (1..32)
    u16 count;           // elements
    u32 flags;
    u32 bit_offset;      // in the report, after the report-ID byte
    i32 logical_min, logical_max;
    u32 application;     // usage of the enclosing application collection
    u32 physical;        // usage of the innermost physical collection (0 if none)
    // Variable fields: element i has usage usages[usage_first + min(i, usage_count - 1)].
    // Array fields: an element's value v selects usages[usage_first + v - logical_min]
    // when a list was given, else usage_min + v - logical_min.
    u16 usage_first, usage_count;
    u32 usage_min, usage_max;
    u16 collection;      // serial number of the innermost collection (fields of one finger share it)
    i32 physical_min, physical_max;
    i8 unit_exponent;
    u32 unit;

    bool is_variable() const { return flags & FLAG_VARIABLE; }
    bool is_relative() const { return flags & FLAG_RELATIVE; }
    bool is_constant() const { return flags & FLAG_CONSTANT; }
};

struct ReportDescriptor {
    static constexpr int MAX_FIELDS = 128, MAX_USAGES = 1024, MAX_APPLICATIONS = 8;
    Field fields[MAX_FIELDS];
    int field_count = 0;
    u32 usages[MAX_USAGES];
    int usage_count = 0;
    bool uses_report_ids = false;
    u32 applications[MAX_APPLICATIONS]; // top-level application collections, in order
    int application_count = 0;
    u32 max_input_bits = 0;             // longest input report (without the ID byte)
};

// Parse a report descriptor. 0 or -EINVAL (malformed / too big for our tables).
long parse(const u8* data, usize length, ReportDescriptor* out);

// Linux key code for a usage on the keyboard page (0 = none).
u16 keyboard_usage_to_key(u8 usage_id);

// Read `size` bits at `bit_offset` (little-endian bit order, as HID reports are).
u32 extract(const u8* report, u32 report_bytes, u32 bit_offset, u8 size);

// Where HidInput sends its events (the driver: the real evdev devices; the
// self-test: a recorder). `app` indexes HidInput::devices.
class EventSink {
public:
    virtual void event(int app, u16 type, u16 code, i32 value) = 0;
};

// Translation of one HID interface's input reports into evdev events. Each
// top-level application collection becomes its own input device (like
// Linux: "<name> Mouse", "<name> Consumer Control", ...).
class HidInput {
public:
    static constexpr int MAX_DEVICES = ReportDescriptor::MAX_APPLICATIONS;
    ~HidInput();

    // Parse the descriptor and work out every device's capabilities.
    // Returns the number of input devices with something to report (0 = useless).
    int init(const u8* descriptor, usize length, EventSink* sink);

    // One input report as received from the interrupt endpoint.
    void process(const u8* report, u32 length);

    // Bytes of the longest input report, including the report-ID byte.
    u32 max_report_bytes() const;

    input::Device devices[MAX_DEVICES]; // capabilities + names; registered by the driver
    bool device_used[MAX_DEVICES] = {};
    int device_count = 0;
    bool is_keyboard[MAX_DEVICES] = {};  // feeds the text console too
    ReportDescriptor desc;

private:
    struct Mapping {
        u16 type = 0, code = 0; // type 0: unmapped
        bool hat = false;
    };
    bool map_usage(u32 usage, const Field& f, Mapping* m) const;
    int device_of(const Field& f) const;
    void declare(int dev, const Field& f, const Mapping& m);
    void emit(int dev, u16 type, u16 code, i32 value);
    void process_variable(int fi, const u8* data, u32 bytes);
    void process_array(int fi, const u8* data, u32 bytes);
    u32 array_usage(const Field& f, i32 value) const;

    EventSink* m_sink = nullptr;
    // Last value of every input element (variable: value; array: selected usage).
    i32* m_state[ReportDescriptor::MAX_FIELDS] = {};
    bool m_dirty[MAX_DEVICES] = {};
};

} // namespace usb::hid
