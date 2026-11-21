#pragma once

#include <drivers/input/input.hpp>

// Turns a multi-touch touchpad's finger positions into mouse events, so it
// works with any program that understands a mouse (X's evdev driver):
//   one finger moves the pointer (with acceleration), two fingers scroll,
//   pressing the pad clicks (with two fingers down, or on a clickpad in its
//   bottom-right corner: right click),
//   a quick tap clicks (one finger: left, two: right, three: middle).
// Drivers (Apple bcm5974, PS/2 Synaptics/Elantech, HID precision touchpads
// over USB or I2C) feed frames.
namespace input {

class TouchpadMouse {
public:
    struct Finger {
        i32 x, y; // device units, y growing upwards
    };

    // units_per_mm: the pad's resolution (scales pointer speed and the
    // tap/scroll thresholds).
    void init(const char* name, u16 bustype, u16 vendor, u16 product, i32 units_per_mm);
    // The pad's coordinate range; a clickpad (the whole pad is the button)
    // then right-clicks in its bottom-right corner, like Windows.
    void set_area(i32 x_min, i32 x_max, i32 y_min, i32 y_max, bool clickpad);
    // One frame: `count` fingers touch, `positions` (<= count) of them with
    // known positions (simple pads report one position for several fingers).
    void frame(const Finger* fingers, int positions, int count, bool button);
    void frame(const Finger* fingers, int count, bool button) { frame(fingers, count, count, button); }
    // A separate button (a pad's physical left/right/middle button, not the clickpad).
    void button(u16 code, bool down);
    Device& device() { return m_dev; }
    bool registered() const { return m_registered; }

private:
    bool in_right_button_area(const Finger& f) const;

    Device m_dev;
    bool m_registered = false;
    i32 m_upm = 40;
    i32 m_x_min = 0, m_x_max = 0, m_y_min = 0, m_y_max = 0;
    bool m_clickpad = false;
    int m_last_count = 0;
    i32 m_last_x = 0, m_last_y = 0;    // centroid of the last frame
    i32 m_frac_x = 0, m_frac_y = 0;    // sub-pixel remainders (x 256)
    i32 m_scroll_x = 0, m_scroll_y = 0;
    bool m_button = false;
    u16 m_button_code = 0;
    // Tap detection.
    u64 m_touch_start = 0;
    int m_touch_max_fingers = 0;
    i32 m_touch_travel = 0;
    bool m_touch_clicked = false;
};

// Decoders of every touchpad protocol checked with sample packets (boot self-test).
void run_touchpad_selftest();

} // namespace input
