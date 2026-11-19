#include <drivers/input/touchpad.hpp>
#include <lib/string.hpp>
#include <time/clock.hpp>

namespace input {

namespace {

i32 iabs(i32 v) { return v < 0 ? -v : v; }

} // namespace

void TouchpadMouse::init(const char* name, u16 bustype, u16 vendor, u16 product, i32 units_per_mm) {
    for (usize i = 0; name[i] && i < sizeof(m_dev.name) - 1; i++)
        m_dev.name[i] = name[i];
    m_dev.bustype = bustype;
    m_dev.vendor = vendor;
    m_dev.product = product;
    m_dev.ev_bits = (1ull << EV_KEY) | (1ull << EV_REL);
    m_dev.rel_bits = (1ull << REL_X) | (1ull << REL_Y) | (1ull << REL_WHEEL) | (1ull << REL_HWHEEL);
    m_dev.set_key(BTN_LEFT);
    m_dev.set_key(BTN_RIGHT);
    m_dev.set_key(BTN_MIDDLE);
    m_dev.props = 1u << INPUT_PROP_POINTER;
    m_upm = units_per_mm > 0 ? units_per_mm : 40;
    register_device(&m_dev);
}

void TouchpadMouse::frame(const Finger* fingers, int count, bool button) {
    u64 now = clock::now_ns();
    bool synced = false;
    auto emit = [&](u16 type, u16 code, i32 value) {
        m_dev.report(type, code, value);
        synced = true;
    };

    // Physical click: right button when two or more fingers are down.
    if (button != m_button) {
        if (button)
            m_button_code = count >= 2 ? BTN_RIGHT : BTN_LEFT;
        emit(EV_KEY, m_button_code, button ? 1 : 0);
        m_button = button;
        if (button)
            m_touch_clicked = true; // not a tap
    }

    i32 cx = 0, cy = 0;
    for (int i = 0; i < count; i++) {
        cx += fingers[i].x;
        cy += fingers[i].y;
    }
    if (count) {
        cx /= count;
        cy /= count;
    }

    if (count == 0) {
        // Lift: was it a tap? Short, little movement, no physical click.
        if (m_last_count > 0 && !m_touch_clicked && now - m_touch_start < 180'000'000 &&
            m_touch_travel < 2 * m_upm) {
            u16 code = m_touch_max_fingers >= 2 ? BTN_RIGHT : BTN_LEFT;
            emit(EV_KEY, code, 1);
            m_dev.sync();
            emit(EV_KEY, code, 0);
        }
        m_last_count = 0;
        m_scroll_x = m_scroll_y = 0;
    } else {
        if (m_last_count == 0) { // new touch
            m_touch_start = now;
            m_touch_max_fingers = count;
            m_touch_travel = 0;
            m_touch_clicked = m_button;
        }
        if (count > m_touch_max_fingers)
            m_touch_max_fingers = count;
        if (count == m_last_count) {
            i32 dx = cx - m_last_x, dy = cy - m_last_y;
            m_touch_travel += iabs(dx) + iabs(dy);
            if (count == 1 && !(m_button && m_button_code == BTN_RIGHT)) {
                // Pointer: ~8 px per mm, up to 2.5x faster for quick moves.
                i32 speed = (iabs(dx) + iabs(dy)) * 100 / m_upm; // hundredths of a mm this frame
                i32 accel = 256 + (speed > 30 ? (speed - 30) * 4 : 0);
                if (accel > 640)
                    accel = 640;
                m_frac_x += dx * 8 * accel / m_upm;
                m_frac_y += -dy * 8 * accel / m_upm; // screen y grows downwards
                i32 px = m_frac_x / 256, py = m_frac_y / 256;
                m_frac_x -= px * 256;
                m_frac_y -= py * 256;
                if (px)
                    emit(EV_REL, REL_X, px);
                if (py)
                    emit(EV_REL, REL_Y, py);
            } else if (count == 2 && !m_button) {
                // Scroll: one wheel step per ~2.5 mm of two-finger travel.
                m_scroll_y += dy;
                m_scroll_x += dx;
                i32 step = m_upm * 5 / 2;
                while (m_scroll_y >= step) { emit(EV_REL, REL_WHEEL, 1); m_scroll_y -= step; }
                while (m_scroll_y <= -step) { emit(EV_REL, REL_WHEEL, -1); m_scroll_y += step; }
                while (m_scroll_x >= step) { emit(EV_REL, REL_HWHEEL, 1); m_scroll_x -= step; }
                while (m_scroll_x <= -step) { emit(EV_REL, REL_HWHEEL, -1); m_scroll_x += step; }
            }
        } else {
            // Finger count changed: the centroid jumps; start over from here.
            m_frac_x = m_frac_y = 0;
            m_scroll_x = m_scroll_y = 0;
        }
        m_last_x = cx;
        m_last_y = cy;
        m_last_count = count;
    }
    if (synced)
        m_dev.sync();
}

} // namespace input
