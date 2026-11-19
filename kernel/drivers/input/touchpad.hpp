#pragma once

#include <drivers/input/input.hpp>

// Turns a multi-touch touchpad's finger positions into mouse events, so it
// works with any program that understands a mouse (X's evdev driver):
//   one finger moves the pointer (with acceleration), two fingers scroll,
//   pressing the pad clicks (with two fingers down: right click),
//   a quick tap clicks (one finger: left, two: right).
// Drivers (Apple bcm5974, later Synaptics/ELAN/I2C-HID) feed frames.
namespace input {

class TouchpadMouse {
public:
    struct Finger {
        i32 x, y; // device units, y growing upwards
    };

    // units_per_mm: the pad's resolution (scales pointer speed and the
    // tap/scroll thresholds).
    void init(const char* name, u16 bustype, u16 vendor, u16 product, i32 units_per_mm);
    void frame(const Finger* fingers, int count, bool button);
    Device& device() { return m_dev; }

private:
    Device m_dev;
    i32 m_upm = 40;
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

} // namespace input
