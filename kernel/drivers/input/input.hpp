#pragma once

#include <lib/types.hpp>

// Input devices with the Linux evdev interface: /dev/input/eventN, read by
// Wayland compositors (libinput), SDL games, X servers and our desktop.
// Keyboards report Linux key codes, mice relative motion + buttons.
namespace input {

// Event types and codes (Linux values, subset).
constexpr u16 EV_SYN = 0x00, EV_KEY = 0x01, EV_REL = 0x02, EV_ABS = 0x03, EV_MSC = 0x04, EV_REP = 0x14;
constexpr u16 SYN_REPORT = 0;
constexpr u16 REL_X = 0x00, REL_Y = 0x01, REL_HWHEEL = 0x06, REL_WHEEL = 0x08;
constexpr u16 ABS_X = 0x00, ABS_Y = 0x01;
constexpr u16 BTN_LEFT = 0x110, BTN_RIGHT = 0x111, BTN_MIDDLE = 0x112, BTN_SIDE = 0x113, BTN_EXTRA = 0x114;
constexpr u16 KEY_MAX = 0x2FF;
constexpr u16 BUS_I8042 = 0x11, BUS_USB = 0x03, BUS_VIRTUAL = 0x06;

class Device {
public:
    char name[80] = {};
    u16 bustype = 0, vendor = 0, product = 0, version = 1;
    u64 ev_bits = 0;                 // supported event types
    u64 key_bits[KEY_MAX / 64 + 1] = {};
    u64 rel_bits = 0;
    u64 abs_bits = 0;
    i32 abs_min[2] = {}, abs_max[2] = {}; // ABS_X / ABS_Y ranges (tablets)

    void set_key(u16 code) { key_bits[code / 64] |= 1ull << (code % 64); }

    // Queue an event for every reader; sync() ends one atomic report.
    void report(u16 type, u16 code, i32 value);
    void sync() { report(EV_SYN, SYN_REPORT, 0); }

    // Current key state (for EVIOCGKEY).
    u64 key_down[KEY_MAX / 64 + 1] = {};
    u32 index = 0; // N in eventN
};

// Register a device: creates /dev/input/eventN.
void register_device(Device* d);
void init();

} // namespace input
