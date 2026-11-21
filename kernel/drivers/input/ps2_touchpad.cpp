// PS/2 touchpads: Synaptics and Elantech in absolute mode (see ps2_touchpad.hpp).

#include <boot/limine_requests.hpp>
#include <core/log.hpp>
#include <drivers/input/ps2_touchpad.hpp>
#include <lib/string.hpp>
#include <time/clock.hpp>

namespace ps2pad {

namespace {

enum class Kind { None, Synaptics, Elantech };

Kind g_kind = Kind::None;
Port g_port;
SynapticsInfo g_syn;
ElantechState g_elan;
input::TouchpadMouse g_pad;
u8 g_packet[6];
int g_len = 0;
u64 g_last_byte = 0;
u8 g_stick_pt = 0, g_stick_ext = 0; // stick buttons: from its own packets, wired to the pad
bool g_right = false, g_middle = false;

bool cmd(u8 b) { return g_port.command(b); }

// ---- Synaptics ----

// "Special command" encoding: four set-resolution commands carry 8 bits.
bool sliced(u8 arg) {
    for (int shift = 6; shift >= 0; shift -= 2)
        if (!cmd(0xE8) || !cmd((arg >> shift) & 3))
            return false;
    return true;
}

bool status_bytes(u8 out[3]) {
    if (!cmd(0xE9)) // status request: three bytes follow
        return false;
    return g_port.read(&out[0]) && g_port.read(&out[1]) && g_port.read(&out[2]);
}

bool syn_query(u8 q, u8 out[3]) { return sliced(q) && status_bytes(out); }

bool syn_query_int(u8 q, u32* v) {
    u8 r[3];
    if (!syn_query(q, r))
        return false;
    *v = (static_cast<u32>(r[0]) << 16) | (r[1] << 8) | r[2];
    return true;
}

bool synaptics_probe() {
    SynapticsInfo& s = g_syn;
    s = SynapticsInfo{};
    if (!syn_query_int(0x00, &s.identity) || ((s.identity >> 8) & 0xFF) != 0x47)
        return false; // not a Synaptics pad
    u32 major = s.identity & 0x0F, minor = (s.identity >> 16) & 0xFF, full = (major << 8) | minor;
    syn_query_int(0x03, &s.model);
    if (!(s.model & 0x80)) {
        klog::info("PS/2: Synaptics touchpad %u.%u with the old packet format: used as a plain mouse", major, minor);
        return false;
    }
    if (!syn_query_int(0x02, &s.capabilities))
        return false;
    if (!(s.capabilities & (1u << 23))) // capExtended: the rest is valid only with it
        s.capabilities = 0;
    u32 requests = (s.capabilities >> 20) & 7;
    if (requests >= 1)
        syn_query_int(0x09, &s.ext_cap);
    if (requests >= 4)
        syn_query_int(0x0C, &s.ext_cap_0c);
    if (full >= 0x705) {
        u8 bid[3];
        if (syn_query(0x01, bid) && (bid[0] & 0x02))
            syn_query_int(0x10, &s.ext_cap_10);
    }
    u8 r[3];
    if (major >= 4 && syn_query(0x08, r) && r[0] && (r[1] & 0x80) && r[2])
        s.units_per_mm = r[0];
    if (requests >= 5 && (s.ext_cap_0c & (1u << 17)) && syn_query(0x0D, r)) {
        s.x_max = (r[0] << 5) | ((r[1] & 0x0F) << 1);
        s.y_max = (r[2] << 5) | ((r[1] & 0xF0) >> 3);
    }
    if ((s.ext_cap_0c & (1u << 13)) && (requests >= 7 || full == 0x801) && syn_query(0x0F, r)) {
        s.x_min = (r[0] << 5) | ((r[1] & 0x0F) << 1);
        s.y_min = (r[2] << 5) | ((r[1] & 0xF0) >> 3);
    }
    if (s.x_max <= s.x_min || s.y_max <= s.y_min) {
        s.x_min = 1472, s.x_max = 5472, s.y_min = 1408, s.y_max = 4448;
    }
    // Absolute, 80 packets/s, gestures off, W mode (finger count/width).
    u8 mode = 0x80 | 0x40 | 0x04 | ((s.capabilities & (1u << 23)) ? 0x01 : 0);
    if (!sliced(mode) || !cmd(0xF3) || !cmd(0x14)) // the mode byte is committed by "set sample rate 20"
        return false;
    if (s.pass_through()) { // wake the TrackPoint behind it: "enable" through the pass-through port
        sliced(0xF4);
        cmd(0xF3);
        cmd(0x28);
    }
    klog::info("PS/2: Synaptics touchpad %u.%u (caps %06x/%06x/%06x/%06x, x %d..%d, y %d..%d, %d units/mm%s%s%s): "
               "absolute mode",
               major, minor, s.capabilities, s.ext_cap, s.ext_cap_0c, s.ext_cap_10, s.x_min, s.x_max, s.y_min,
               s.y_max, s.units_per_mm, s.clickpad() ? ", clickpad" : "",
               s.pass_through() ? ", TrackPoint pass-through" : "",
               s.ext_buttons_stick() ? ", TrackPoint buttons on the pad" : "");
    return true;
}

// ---- Elantech ----

bool elan_cmd(u8 c, u8 out[3]) { // v3+: "custom command" prefix
    return cmd(0xF8) && cmd(c) && status_bytes(out);
}

bool elan_send(u8 c, u8 out[3]) { return g_elan.hw_version >= 3 ? elan_cmd(c, out) : syn_query(c, out); }

bool elan_write_reg(u8 reg, u8 val) {
    if (g_elan.hw_version == 3)
        return cmd(0xF8) && cmd(0x00) && cmd(0xF8) && cmd(reg) && cmd(0xF8) && cmd(val) && cmd(0xE6);
    // v4
    return cmd(0xF8) && cmd(0x00) && cmd(0xF8) && cmd(reg) && cmd(0xF8) && cmd(0x00) && cmd(0xF8) && cmd(val) &&
           cmd(0xE6);
}

bool elan_signature_valid(const u8* p) {
    static const u8 rates[] = {200, 100, 80, 60, 40, 20, 10};
    if (p[0] == 0)
        return false;
    if (p[1] == 0)
        return true;
    if ((p[0] & 0x0F) >= 0x06 && (p[1] & 0xAF) == 0x0F && p[2] < 40)
        return true;
    for (u8 r : rates)
        if (p[2] == r)
            return false;
    return true;
}

bool elantech_probe() {
    ElantechState& e = g_elan;
    e = ElantechState{};
    u8 p[3];
    // The "magic knock": disable, scaling 1:1 three times, status request.
    cmd(0xF6);
    if (!cmd(0xF5) || !cmd(0xE6) || !cmd(0xE6) || !cmd(0xE6) || !status_bytes(p))
        return false;
    if (p[0] != 0x3C || p[1] != 0x03 || (p[2] != 0xC8 && p[2] != 0x00))
        return false;
    if (!syn_query(0x01, p) || !elan_signature_valid(p))
        return false; // Logitech mice answer the knock too
    e.fw_version = (static_cast<u32>(p[0]) << 16) | (p[1] << 8) | p[2];
    u32 ic = (e.fw_version >> 16) & 0x0F;
    if (e.fw_version < 0x020030 || e.fw_version == 0x020600)
        e.hw_version = 1;
    else if (ic == 2 || ic == 4)
        e.hw_version = 2;
    else if (ic == 5)
        e.hw_version = 3;
    else if (ic >= 6)
        e.hw_version = 4;
    if (e.hw_version < 3) {
        klog::info("PS/2: Elantech touchpad (firmware %06x, hardware version %d): used as a plain mouse",
                   e.fw_version, e.hw_version);
        return false;
    }
    e.crc = (e.fw_version & 0x4000) != 0;
    e.buttonpad = (e.fw_version & 0x001000) != 0;
    u8 caps[3];
    if (!elan_send(0x02, caps) || !elan_send(0x03, e.samples))
        return false;
    if (e.samples[1] == 0x74 && e.hw_version == 3) {
        klog::info("PS/2: Elantech v3 touchpad with a broken absolute mode: used as a plain mouse");
        return false;
    }
    e.trackpoint = caps[0] & 0x80;
    if (e.hw_version == 4 && elan_send(0x04, p)) {
        u32 res = p[1] & 0x0F;
        e.units_per_mm = static_cast<i32>((res * 10 + 790) * 10 / 254);
    }
    if (!elan_send(0x00, p))
        return false;
    e.x_max = ((p[0] & 0x0F) << 8) | p[1];
    e.y_max = ((p[0] & 0xF0) << 4) | p[2];
    if (e.x_max <= 0 || e.y_max <= 0)
        return false;
    bool ok = e.hw_version == 3 ? elan_write_reg(0x10, 0x0B) : elan_write_reg(0x07, 0x01);
    if (!ok) {
        klog::warn("PS/2: Elantech touchpad (firmware %06x): absolute mode refused, used as a plain mouse",
                   e.fw_version);
        return false;
    }
    klog::info("PS/2: Elantech touchpad v%d (firmware %06x, caps %02x%02x%02x, x 0..%d, y 0..%d, %d units/mm%s%s): "
               "absolute mode",
               e.hw_version, e.fw_version, caps[0], caps[1], caps[2], e.x_max, e.y_max, e.units_per_mm,
               e.buttonpad ? ", clickpad" : "", e.trackpoint ? ", TrackPoint" : "");
    return true;
}

// ---- delivery ----

void deliver(const Report& r) {
    switch (r.kind) {
    case Report::Touch:
        if (r.right != g_right) {
            g_pad.button(input::BTN_RIGHT, r.right);
            g_right = r.right;
        }
        if (r.middle != g_middle) {
            g_pad.button(input::BTN_MIDDLE, r.middle);
            g_middle = r.middle;
        }
        g_pad.frame(r.fingers, r.positions, r.count, r.left);
        break;
    case Report::Stick:
        g_stick_pt = r.stick_buttons;
        if (g_port.stick)
            g_port.stick(static_cast<u8>(g_stick_pt | g_stick_ext), r.dx, r.dy);
        break;
    case Report::StickButtons:
        g_stick_ext = r.stick_buttons;
        if (g_port.stick)
            g_port.stick(static_cast<u8>(g_stick_pt | g_stick_ext), 0, 0);
        break;
    default:
        break;
    }
}

} // namespace

// ---- decoders ----

bool synaptics_decode(const SynapticsInfo& s, const u8 p[6], Report* out) {
    *out = Report{};
    u32 w = ((p[0] & 0x30) >> 2) | ((p[0] & 0x04) >> 1) | ((p[3] & 0x04) >> 2);
    if (w == 3 && s.pass_through()) { // TrackPoint: an embedded 3-byte PS/2 packet
        u8 flags = p[1];
        if (flags & 0xC0)
            return false; // overflow
        out->kind = Report::Stick;
        out->stick_buttons = flags & 7;
        out->dx = p[4] - ((flags & 0x10) ? 256 : 0);
        out->dy = -(p[5] - ((flags & 0x20) ? 256 : 0)); // PS/2: up is positive
        return true;
    }
    if (w == 2)
        return false; // advanced-gesture / pen packets: not enabled
    bool ext = s.ext_buttons() && ((p[0] ^ p[3]) & 0x02);
    if (ext && s.ext_buttons_stick()) {
        // ThinkPads since 2015: the TrackPoint's buttons are wired to the
        // pad and come in its packets (bytes 4/5 carry them, not a position).
        u32 bits = (s.ext_buttons() + 1) >> 1, mask = (1u << bits) - 1;
        u32 eb = (p[4] & mask) | ((p[5] & mask) << bits);
        out->kind = Report::StickButtons;
        out->stick_buttons = static_cast<u8>((eb & 1) | ((eb >> 2) & 1) << 1 | ((eb >> 1) & 1) << 2);
        return true;
    }
    i32 x = ((p[3] & 0x10) << 8) | ((p[1] & 0x0F) << 8) | p[4];
    i32 y = ((p[3] & 0x20) << 7) | ((p[1] & 0xF0) << 4) | p[5];
    // 13-bit values: the top ones wrap to negative; 8176 means "at the edge".
    if (x > 8176) x -= 1 << 13; else if (x == 8176) x = s.x_max;
    if (y > 8176) y -= 1 << 13; else if (y == 8176) y = s.y_max;
    u32 z = p[2];
    out->kind = Report::Touch;
    if (s.clickpad()) {
        out->left = (p[0] ^ p[3]) & 0x01; // the pad's click comes as the "middle" bit
    } else {
        out->left = p[0] & 0x01;
        out->right = p[0] & 0x02;
        if (s.middle_button())
            out->middle = (p[0] ^ p[3]) & 0x01;
    }
    if (z < 25) { // no finger (or hovering)
        out->count = 0;
        return true;
    }
    int fingers = 1;
    if (s.multifinger() && w == 0)
        fingers = 2;
    else if (s.multifinger() && w == 1)
        fingers = 3;
    out->fingers[0] = {x, y}; // Synaptics' y grows upwards already
    out->positions = 1;
    out->count = fingers;
    return true;
}

namespace {

// A TrackPoint packet (a valid packet even when its contents are dropped).
bool elan_trackpoint(const u8 p[6], Report* out) {
    u32 t = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24);
    switch (t & ~7u) {
    case 0x06000030u: case 0x16008020u: case 0x26800010u: case 0x36808000u:
        // Firmware sometimes misreports: values outside -127..127 are dropped.
        if (p[4] == 0x80 || p[5] == 0x80 || p[1] >> 7 == p[4] >> 7 || p[2] >> 7 == p[5] >> 7)
            return true;
        out->kind = Report::Stick;
        out->stick_buttons = p[0] & 7;
        out->dx = p[4] - static_cast<i32>((p[1] ^ 0x80) << 1);
        out->dy = static_cast<i32>((p[2] ^ 0x80) << 1) - p[5];
        return true;
    default:
        return true;
    }
}

void elan_buttons(const ElantechState& s, const u8 p[6], Report* out) {
    if (s.buttonpad) {
        out->left = p[0] & 0x03;
    } else {
        out->left = p[0] & 0x01;
        out->right = p[0] & 0x02;
    }
}

} // namespace

bool elantech_decode(ElantechState& s, const u8 p[6], Report* out) {
    *out = Report{};
    if (s.hw_version == 3) {
        static const u8 debounce[] = {0xC4, 0xFF, 0xFF, 0x02, 0xFF, 0xFF};
        if (memcmp(p, debounce, 6) == 0)
            return true; // nothing to report
        bool head, tail;
        if (s.crc) {
            head = (p[3] & 0x09) == 0x08;
            tail = (p[3] & 0x09) == 0x09;
        } else {
            head = (p[0] & 0x0C) == 0x04 && (p[3] & 0xCF) == 0x02;
            tail = (p[0] & 0x0C) == 0x0C && (p[3] & 0xCE) == 0x0C;
            if (!head && !tail && (p[3] & 0x0F) == 0x06)
                return elan_trackpoint(p, out);
        }
        if (!head && !tail)
            return false;
        int fingers = (p[0] & 0xC0) >> 6;
        i32 x = ((p[1] & 0x0F) << 8) | p[2], y = ((p[4] & 0x0F) << 8) | p[5];
        if (fingers == 2 && head) {
            s.mt[0] = {x, y}; // the second finger follows in the tail packet
            return true;
        }
        out->kind = Report::Touch;
        elan_buttons(s, p, out);
        out->count = fingers;
        if (fingers == 2) {
            out->fingers[0] = s.mt[0];
            out->fingers[1] = {x, y};
            out->positions = 2;
        } else if (fingers) {
            out->fingers[0] = {x, y};
            out->positions = 1;
        }
        return true;
    }
    if (s.hw_version != 4)
        return false;
    if (s.trackpoint && (p[3] & 0x0F) == 0x06)
        return elan_trackpoint(p, out);
    u32 ic = (s.fw_version >> 16) & 0x0F;
    bool sane;
    if (s.crc)
        sane = (p[3] & 0x08) == 0;
    else if (ic == 7 && s.samples[1] == 0x2A)
        sane = (p[3] & 0x1C) == 0x10;
    else
        sane = (p[0] & 0x08) == 0 && (p[3] & 0x1C) == 0x10;
    if (!sane)
        return false;
    switch (p[3] & 0x03) {
    case 0: // status: which fingers are down
        s.present &= p[1] & 0x1F;
        break;
    case 1: { // head: one finger's absolute position
        int id = ((p[3] & 0xE0) >> 5) - 1;
        if (id < 0 || id >= 5)
            return false;
        s.mt[id] = {((p[1] & 0x0F) << 8) | p[2], ((p[4] & 0x0F) << 8) | p[5]};
        s.present |= 1u << id;
        break;
    }
    case 2: { // motion: deltas of up to two fingers
        int id = ((p[0] & 0xE0) >> 5) - 1, sid = ((p[3] & 0xE0) >> 5) - 1;
        if (id < 0 || id >= 5)
            return false;
        i32 weight = (p[0] & 0x10) ? 5 : 1;
        s.mt[id].x += static_cast<i8>(p[1]) * weight;
        s.mt[id].y += static_cast<i8>(p[2]) * weight;
        if (sid >= 0 && sid < 5) {
            s.mt[sid].x += static_cast<i8>(p[4]) * weight;
            s.mt[sid].y += static_cast<i8>(p[5]) * weight;
        }
        break;
    }
    default:
        return false;
    }
    out->kind = Report::Touch;
    elan_buttons(s, p, out);
    for (int i = 0; i < 5; i++)
        if (s.present & (1u << i))
            out->fingers[out->positions++] = s.mt[i];
    out->count = out->positions;
    return true;
}

// ---- probing and the byte stream ----

bool probe(const Port& port) {
    g_port = port;
    g_kind = Kind::None;
    if (strstr(boot::cmdline(), "psmouse=basic"))
        return false;
    const char* name = nullptr;
    i32 upm = 0, x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    bool clickpad = false;
    if (synaptics_probe()) {
        g_kind = Kind::Synaptics;
        name = "SynPS/2 Synaptics TouchPad";
        upm = g_syn.units_per_mm;
        x0 = g_syn.x_min, x1 = g_syn.x_max, y0 = g_syn.y_min, y1 = g_syn.y_max;
        clickpad = g_syn.clickpad();
    } else {
        // A failed Synaptics query leaves the mouse confused: start over.
        cmd(0xF6);
        if (elantech_probe()) {
            g_kind = Kind::Elantech;
            name = "ETPS/2 Elantech Touchpad";
            upm = g_elan.units_per_mm;
            x1 = g_elan.x_max, y1 = g_elan.y_max;
            clickpad = g_elan.buttonpad;
        } else {
            cmd(0xF6); // back to defaults for the plain-mouse setup
            return false;
        }
    }
    if (!g_pad.registered()) // probing runs again after the USB handoff
        g_pad.init(name, input::BUS_I8042, 2, g_kind == Kind::Synaptics ? 7 : 0x18, upm);
    g_pad.set_area(x0, x1, y0, y1, clickpad);
    g_len = 0;
    return true;
}

bool active() { return g_kind != Kind::None; }

void byte(u8 b) {
    u64 now = clock::now_ns();
    if (g_len && now - g_last_byte > 100'000'000)
        g_len = 0; // a packet never finished: resynchronize
    g_last_byte = now;
    if (g_kind == Kind::Synaptics) {
        // Sync: byte 0 is 10xx0xxx, byte 3 is 11xx0xxx.
        if ((g_len == 0 && (b & 0xC8) != 0x80) || (g_len == 3 && (b & 0xC8) != 0xC0)) {
            g_len = 0;
            return;
        }
    }
    g_packet[g_len++] = b;
    if (g_len < 6)
        return;
    Report r;
    bool ok = g_kind == Kind::Synaptics ? synaptics_decode(g_syn, g_packet, &r) : elantech_decode(g_elan, g_packet, &r);
    if (!ok && g_kind == Kind::Elantech) {
        // Out of step (or a packet type we don't know): slide by one byte.
        memmove(g_packet, g_packet + 1, 5);
        g_len = 5;
        return;
    }
    g_len = 0;
    if (ok)
        deliver(r);
}

} // namespace ps2pad
