// HID precision touchpads (see hid_touchpad.hpp).

#include <core/log.hpp>
#include <drivers/usb/hid_touchpad.hpp>
#include <lib/string.hpp>

namespace usb::hid {

namespace {

constexpr u32 GD_X = usage(1, 0x30), GD_Y = usage(1, 0x31);
constexpr u32 DG_TIP = usage(0x0D, 0x42), DG_CONFIDENCE = usage(0x0D, 0x47), DG_CONTACT_ID = usage(0x0D, 0x51),
              DG_CONTACT_COUNT = usage(0x0D, 0x54), DG_INPUT_MODE = usage(0x0D, 0x52),
              DG_SURFACE_SWITCH = usage(0x0D, 0x57), DG_BUTTON_SWITCH = usage(0x0D, 0x58),
              DG_LATENCY_MODE = usage(0x0D, 0x60);
constexpr u32 BUTTON_1 = usage(PAGE_BUTTON, 1);

u32 element_usage(const ReportDescriptor& d, const Field& f, u16 i) {
    if (!f.usage_count)
        return 0;
    return d.usages[f.usage_first + (i < f.usage_count ? i : f.usage_count - 1)];
}

// Bytes of feature report `id` (with the ID byte).
u32 feature_bytes(const ReportDescriptor& d, u8 id) {
    u32 bits = 0;
    for (int i = 0; i < d.field_count; i++) {
        const Field& f = d.fields[i];
        if (f.kind == ReportKind::Feature && f.report_id == id) {
            u32 end = f.bit_offset + static_cast<u32>(f.size) * f.count;
            if (end > bits)
                bits = end;
        }
    }
    return (bits + 7) / 8 + (d.uses_report_ids ? 1 : 0);
}

// The pad's size in mm from a coordinate field's physical extent.
i32 size_mm(const Field& f) {
    i32 phys = f.physical_max - f.physical_min;
    if (phys <= 0)
        return 0;
    // Units: 0x11 = centimetres, 0x13 = inches (linear systems).
    i64 um; // micrometres x 10^-exponent handled below
    if ((f.unit & 0xFF) == 0x11)
        um = static_cast<i64>(phys) * 10000;
    else if ((f.unit & 0xFF) == 0x13)
        um = static_cast<i64>(phys) * 25400;
    else
        return 0;
    for (i8 e = f.unit_exponent; e > 0; e--)
        um *= 10;
    for (i8 e = f.unit_exponent; e < 0; e++)
        um /= 10;
    return static_cast<i32>(um / 1000);
}

} // namespace

void insert(u8* report, u32 report_bytes, u32 bit_offset, u8 size, u32 value) {
    for (u8 i = 0; i < size; i++) {
        u32 bit = bit_offset + i;
        if (bit / 8 >= report_bytes)
            break;
        if (value & (1u << i))
            report[bit / 8] |= static_cast<u8>(1u << (bit % 8));
        else
            report[bit / 8] &= static_cast<u8>(~(1u << (bit % 8)));
    }
}

bool Touchpad::init(const ReportDescriptor& d) {
    m_desc = &d;
    contacts = 0;
    bool have_app = false;
    for (int i = 0; i < d.application_count; i++)
        have_app |= d.applications[i] == DG_TOUCHPAD;
    if (!have_app)
        return false;
    bool separate_buttons = false;
    for (int fi = 0; fi < d.field_count; fi++) {
        const Field& f = d.fields[fi];
        if (f.is_constant())
            continue;
        for (u16 e = 0; e < f.count && e < 64; e++) {
            u32 u = element_usage(d, f, e);
            Loc loc{static_cast<i16>(fi), e};
            if (f.kind == ReportKind::Feature) {
                if (u == DG_INPUT_MODE && !m_input_mode.valid()) m_input_mode = loc;
                else if (u == DG_SURFACE_SWITCH) m_surface_switch = loc;
                else if (u == DG_BUTTON_SWITCH) m_button_switch = loc;
                else if (u == DG_LATENCY_MODE) m_latency = loc;
                continue;
            }
            if (f.kind != ReportKind::Input || f.application != DG_TOUCHPAD || !f.is_variable())
                continue;
            if (u == DG_CONTACT_COUNT) {
                m_contact_count = loc;
                continue;
            }
            if ((u >> 16) == PAGE_BUTTON) {
                if (u == BUTTON_1)
                    m_button = loc;
                else
                    separate_buttons = true;
                continue;
            }
            Loc Slot::*member = u == GD_X ? &Slot::x : u == GD_Y ? &Slot::y : u == DG_TIP ? &Slot::tip
                              : u == DG_CONFIDENCE ? &Slot::confidence : u == DG_CONTACT_ID ? &Slot::id : nullptr;
            if (!member)
                continue;
            // A finger is one collection; a second X in the same one starts the next finger.
            int s = 0;
            while (s < contacts && (m_slots[s].collection != f.collection || (m_slots[s].*member).valid()))
                s++;
            if (s == contacts) {
                if (contacts == MAX_CONTACTS)
                    continue;
                m_slots[contacts++] = Slot{f.collection, {}, {}, {}, {}, {}};
            }
            m_slots[s].*member = loc;
            if (f.report_id)
                m_report_id = f.report_id;
        }
    }
    // Only complete fingers count.
    int n = 0;
    for (int s = 0; s < contacts; s++)
        if (m_slots[s].x.valid() && m_slots[s].y.valid() && m_slots[s].tip.valid())
            m_slots[n++] = m_slots[s];
    contacts = n;
    if (!contacts)
        return false;
    const Field& fx = d.fields[m_slots[0].x.field];
    const Field& fy = d.fields[m_slots[0].y.field];
    x_min = fx.logical_min, x_max = fx.logical_max;
    y_min = fy.logical_min, y_max = fy.logical_max;
    i32 width = size_mm(fx);
    if (width < 30 || width > 300)
        width = 100; // unknown: a typical laptop pad
    units_per_mm = (x_max - x_min) / width;
    if (units_per_mm < 1)
        units_per_mm = 1;
    clickpad = !separate_buttons;
    return true;
}

i32 Touchpad::read(const Loc& l, const u8* data, u32 bytes) const {
    const Field& f = m_desc->fields[l.field];
    u32 raw = extract(data, bytes, f.bit_offset + l.elem * f.size, f.size);
    if (f.logical_min < 0 && f.size < 32 && (raw & (1u << (f.size - 1))))
        return static_cast<i32>(raw | ~((1u << f.size) - 1));
    return static_cast<i32>(raw);
}

bool Touchpad::configure(FeatureIo& io) {
    if (!m_input_mode.valid())
        return false;
    const ReportDescriptor& d = *m_desc;
    // Every feature report holding one of the settings, each sent once.
    const Loc* settings[4] = {&m_input_mode, &m_surface_switch, &m_button_switch, &m_latency};
    const u32 values[4] = {3, 1, 1, 0}; // touchpad mode; surface + buttons on; normal latency
    bool done[4] = {};
    bool ok = true;
    for (int i = 0; i < 4; i++) {
        if (!settings[i]->valid() || done[i])
            continue;
        u8 id = d.fields[settings[i]->field].report_id;
        u32 len = feature_bytes(d, id);
        if (len == 0 || len > 64)
            continue;
        u8 buf[64] = {};
        // Start from the device's current values (fields we don't touch keep them).
        long got = io.get_feature(id, buf, len);
        if (got < static_cast<long>(len))
            memset(buf, 0, sizeof(buf));
        u32 off = d.uses_report_ids ? 1 : 0;
        if (off)
            buf[0] = id;
        for (int j = i; j < 4; j++) {
            if (!settings[j]->valid() || d.fields[settings[j]->field].report_id != id)
                continue;
            const Field& f = d.fields[settings[j]->field];
            insert(buf + off, len - off, f.bit_offset + settings[j]->elem * f.size, f.size, values[j]);
            done[j] = true;
        }
        long r = io.set_feature(id, buf, len);
        if (r < 0) {
            klog::warn("hid-touchpad: setting feature report %u failed (%ld)", id, r);
            if (i == 0)
                ok = false;
        }
    }
    return ok;
}

bool Touchpad::process(const u8* report, u32 length, Frame* out) {
    u8 id = 0;
    if (m_desc->uses_report_ids) {
        if (length < 1)
            return false;
        id = report[0];
        report++;
        length--;
    }
    if (id != m_report_id)
        return false;
    if (m_contact_count.valid()) {
        i32 cc = read(m_contact_count, report, length);
        if (cc > 0 || m_expected == 0) { // a non-zero count starts a frame
            m_expected = cc > MAX_CONTACTS ? MAX_CONTACTS : cc;
            m_received = 0;
            m_frame = Frame{};
        }
    } else {
        m_expected = contacts;
        m_received = 0;
        m_frame = Frame{};
    }
    for (int s = 0; s < contacts && m_received < m_expected; s++, m_received++) {
        const Slot& sl = m_slots[s];
        bool tip = read(sl.tip, report, length) != 0;
        bool confident = !sl.confidence.valid() || read(sl.confidence, report, length) != 0;
        if (!tip || !confident) // lifted, or a palm
            continue;
        i32 x = read(sl.x, report, length), y = read(sl.y, report, length);
        m_frame.fingers[m_frame.count++] = {x, y_min + y_max - y}; // HID y grows downwards
    }
    if (m_button.valid())
        m_frame.button |= read(m_button, report, length) != 0;
    if (m_received < m_expected)
        return false; // more of this frame follows
    *out = m_frame;
    m_expected = m_received = 0;
    m_frame = Frame{};
    return true;
}

} // namespace usb::hid
