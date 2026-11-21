#pragma once

#include <drivers/input/touchpad.hpp>
#include <lib/types.hpp>

// PS/2 touchpads in their native absolute mode: Synaptics (most laptops
// until ~2018, ThinkPads with the TrackPoint behind it) and Elantech
// (hardware versions 3 and 4: most Elan PS/2 pads since 2011). Packets are
// decoded into finger frames for TouchpadMouse (pointer, two-finger scroll,
// tap to click); a TrackPoint behind the pad comes out as stick motion.
// Protocol facts as documented by Linux's psmouse drivers (synaptics.c,
// elantech.c); our own code.
namespace ps2pad {

struct Port {
    bool (*command)(u8 b);  // a byte to the aux device; true when it ACKs
    bool (*read)(u8* out);  // a response byte (short timeout)
    // Pointing stick: buttons bit 0 left, 1 right, 2 middle; dy grows downwards.
    void (*stick)(u8 buttons, i32 dx, i32 dy);
};

// Probe for a Synaptics, then an Elantech touchpad and switch it to
// absolute mode (polled, interrupts off; no sleeping). False: a plain
// PS/2 mouse (or a pad we don't speak), which keeps working as a mouse.
bool probe(const Port& port);
bool active();
// One byte from the aux port while active().
void byte(u8 b);

// ---- decoders (pure; also used by the boot self-test) ----

struct Report {
    enum Kind : u8 { None, Touch, Stick, StickButtons } kind = None;
    input::TouchpadMouse::Finger fingers[5] = {};
    int positions = 0, count = 0;
    bool left = false, right = false, middle = false; // pad buttons (clickpad: left)
    u8 stick_buttons = 0;
    i32 dx = 0, dy = 0; // stick motion, dy growing downwards
};

struct SynapticsInfo {
    u32 identity = 0, model = 0, capabilities = 0, ext_cap = 0, ext_cap_0c = 0, ext_cap_10 = 0;
    i32 x_min = 1472, x_max = 5472, y_min = 1408, y_max = 4448, units_per_mm = 60;
    bool clickpad() const { return ext_cap_0c & (1u << 20); }
    bool multifinger() const { return capabilities & (1u << 1); }
    bool middle_button() const { return capabilities & (1u << 18); }
    bool pass_through() const { return capabilities & (1u << 7); }
    u32 ext_buttons() const { u32 n = (ext_cap >> 12) & 0xF; return n > 8 ? 0 : n; }
    bool ext_buttons_stick() const { return ext_cap_10 & (1u << 16); }
};
bool synaptics_decode(const SynapticsInfo& info, const u8 p[6], Report* out);

struct ElantechState {
    u32 fw_version = 0;
    int hw_version = 0;
    bool crc = false, buttonpad = false, trackpoint = false;
    u8 samples[3] = {};
    i32 x_max = 0, y_max = 0, units_per_mm = 31;
    input::TouchpadMouse::Finger mt[5] = {}; // v3/v4: per-finger positions (y growing upwards)
    u8 present = 0;                          // v4: fingers down (bit per id)
};
bool elantech_decode(ElantechState& s, const u8 p[6], Report* out);

} // namespace ps2pad
