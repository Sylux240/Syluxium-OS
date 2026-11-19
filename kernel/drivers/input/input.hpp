#pragma once

#include <lib/types.hpp>

// Input devices with the Linux evdev interface: /dev/input/eventN, read by
// Wayland compositors (libinput), SDL games, X servers and our desktop.
// Keyboards report Linux key codes, mice relative motion + buttons.
namespace input {

// Event types and codes (Linux values, subset).
constexpr u16 EV_SYN = 0x00, EV_KEY = 0x01, EV_REL = 0x02, EV_ABS = 0x03, EV_MSC = 0x04, EV_REP = 0x14;
constexpr u16 SYN_REPORT = 0;
constexpr u16 REL_X = 0x00, REL_Y = 0x01, REL_Z = 0x02, REL_RX = 0x03, REL_RY = 0x04, REL_RZ = 0x05,
              REL_HWHEEL = 0x06, REL_DIAL = 0x07, REL_WHEEL = 0x08, REL_MISC = 0x09, REL_WHEEL_HI_RES = 0x0B,
              REL_HWHEEL_HI_RES = 0x0C;
constexpr u16 ABS_X = 0x00, ABS_Y = 0x01, ABS_Z = 0x02, ABS_RX = 0x03, ABS_RY = 0x04, ABS_RZ = 0x05,
              ABS_THROTTLE = 0x06, ABS_RUDDER = 0x07, ABS_WHEEL = 0x08, ABS_GAS = 0x09, ABS_BRAKE = 0x0A,
              ABS_HAT0X = 0x10, ABS_HAT0Y = 0x11, ABS_PRESSURE = 0x18, ABS_DISTANCE = 0x19, ABS_TILT_X = 0x1A,
              ABS_TILT_Y = 0x1B, ABS_MISC = 0x28, ABS_MAX = 0x3F;
constexpr u16 MSC_SCAN = 0x04;
constexpr u16 BTN_MISC = 0x100, BTN_LEFT = 0x110, BTN_RIGHT = 0x111, BTN_MIDDLE = 0x112, BTN_SIDE = 0x113,
              BTN_EXTRA = 0x114, BTN_MOUSE = 0x110, BTN_JOYSTICK = 0x120, BTN_GAMEPAD = 0x130,
              BTN_SOUTH = 0x130, BTN_EAST = 0x131, BTN_C = 0x132, BTN_NORTH = 0x133, BTN_WEST = 0x134,
              BTN_Z = 0x135, BTN_TL = 0x136, BTN_TR = 0x137, BTN_TL2 = 0x138, BTN_TR2 = 0x139,
              BTN_SELECT = 0x13A, BTN_START = 0x13B, BTN_MODE = 0x13C, BTN_THUMBL = 0x13D, BTN_THUMBR = 0x13E,
              BTN_DIGI = 0x140, BTN_TOOL_PEN = 0x140, BTN_TOOL_FINGER = 0x145, BTN_TOUCH = 0x14A,
              BTN_STYLUS = 0x14B, BTN_STYLUS2 = 0x14C, BTN_DPAD_UP = 0x220, BTN_DPAD_DOWN = 0x221,
              BTN_DPAD_LEFT = 0x222, BTN_DPAD_RIGHT = 0x223, BTN_TRIGGER_HAPPY = 0x2C0;
constexpr u16 KEY_MUTE = 113, KEY_VOLUMEDOWN = 114, KEY_VOLUMEUP = 115, KEY_POWER = 116, KEY_PLAYPAUSE = 164,
              KEY_NEXTSONG = 163, KEY_PREVIOUSSONG = 165, KEY_STOPCD = 166;
constexpr u16 KEY_MAX = 0x2FF;
// Device properties (EVIOCGPROP).
constexpr u32 INPUT_PROP_POINTER = 0, INPUT_PROP_DIRECT = 1;
constexpr u16 BUS_I8042 = 0x11, BUS_USB = 0x03, BUS_VIRTUAL = 0x06;

class Device {
public:
    char name[80] = {};
    u16 bustype = 0, vendor = 0, product = 0, version = 1;
    u64 ev_bits = 0;                 // supported event types
    u64 key_bits[KEY_MAX / 64 + 1] = {};
    u64 rel_bits = 0;
    u64 abs_bits = 0;
    i32 abs_min[ABS_MAX + 1] = {}, abs_max[ABS_MAX + 1] = {}; // ranges of the absolute axes
    i32 abs_flat[ABS_MAX + 1] = {}, abs_res[ABS_MAX + 1] = {}; // dead zone, units per mm (or radian)
    u32 props = 0;                   // INPUT_PROP_* bits
    u64 msc_bits = 0;

    void set_key(u16 code) { key_bits[code / 64] |= 1ull << (code % 64); }
    bool has_key(u16 code) const { return key_bits[code / 64] & (1ull << (code % 64)); }
    void set_abs(u16 axis, i32 min, i32 max, i32 flat = 0) {
        ev_bits |= 1ull << EV_ABS;
        abs_bits |= 1ull << axis;
        abs_min[axis] = min;
        abs_max[axis] = max;
        abs_flat[axis] = flat;
    }
    void set_rel(u16 axis) {
        ev_bits |= 1ull << EV_REL;
        rel_bits |= 1ull << axis;
    }

    // Queue an event for every reader; sync() ends one atomic report.
    void report(u16 type, u16 code, i32 value);
    void sync() { report(EV_SYN, SYN_REPORT, 0); }

    // Current key state (for EVIOCGKEY).
    u64 key_down[KEY_MAX / 64 + 1] = {};
    i32 abs_value[ABS_MAX + 1] = {};  // current axis values (for EVIOCGABS)
    u32 index = 0; // N in eventN
    u8 mirror_pending = 0; // bit 0: keyboard events, bit 1: pointer events since the last sync
};

// Register a device: creates /dev/input/eventN.
void register_device(Device* d);

// The device was unplugged: removes /dev/input/eventN; open files see
// ENODEV (like Linux evdev). No report() may run concurrently or later; the
// caller may free `d` afterwards.
void unregister_device(Device* d);
void init();

} // namespace input
