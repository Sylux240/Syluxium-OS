// HID report-descriptor parser and the HID usage -> Linux input event mapping.
// The mapping follows Linux's drivers/hid/hid-input.c (hidinput_configure_usage).

#include <drivers/usb/hid_parser.hpp>
#include <lib/errno.hpp>
#include <lib/string.hpp>
#include <cxx/runtime.hpp>
#include <mm/heap.hpp>

namespace usb::hid {

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

namespace {

constexpr u8 TYPE_MAIN = 0, TYPE_GLOBAL = 1, TYPE_LOCAL = 2;
constexpr u8 MAIN_INPUT = 8, MAIN_OUTPUT = 9, MAIN_COLLECTION = 10, MAIN_FEATURE = 11, MAIN_END_COLLECTION = 12;
constexpr u8 GLOBAL_USAGE_PAGE = 0, GLOBAL_LOGICAL_MIN = 1, GLOBAL_LOGICAL_MAX = 2, GLOBAL_PHYSICAL_MIN = 3,
             GLOBAL_PHYSICAL_MAX = 4, GLOBAL_UNIT_EXPONENT = 5, GLOBAL_UNIT = 6, GLOBAL_REPORT_SIZE = 7, GLOBAL_REPORT_ID = 8, GLOBAL_REPORT_COUNT = 9,
             GLOBAL_PUSH = 10, GLOBAL_POP = 11;
constexpr u8 LOCAL_USAGE = 0, LOCAL_USAGE_MIN = 1, LOCAL_USAGE_MAX = 2;
constexpr u8 COLLECTION_PHYSICAL = 0, COLLECTION_APPLICATION = 1;

// Values a local usage item may leave without a page: the page that applies
// is the one current when the main item comes (HID 1.11, 6.2.2.8).
constexpr u32 IMPLICIT_PAGE = 0x80000000u;

struct Globals {
    u16 usage_page = 0;
    i32 logical_min = 0, logical_max = 0;
    u32 logical_max_raw = 0;
    u8 logical_max_size = 0;
    u32 report_size = 0, report_count = 0;
    u8 report_id = 0;
    i32 physical_min = 0, physical_max = 0;
    i8 unit_exponent = 0;
    u32 unit = 0;
};

struct Collection {
    u8 type;
    u32 usage;
    u16 serial;
};

struct Parser {
    ReportDescriptor* out;
    Globals g;
    Globals stack[4];
    int stack_depth = 0;
    // Locals (reset after every main item).
    u32 usages[256];
    int usage_count = 0;
    u32 usage_min = 0, usage_max = 0;
    bool have_min = false, have_max = false;
    Collection collections[16];
    int collection_depth = 0;
    u16 collection_serial = 0;
    u32 bit_offset[3][256] = {}; // per report kind and ID

    u32 resolve(u32 u) const {
        return (u & IMPLICIT_PAGE) ? (static_cast<u32>(g.usage_page) << 16) | (u & 0xFFFF) : u;
    }

    void reset_locals() {
        usage_count = 0;
        have_min = have_max = false;
        usage_min = usage_max = 0;
    }

    u32 application() const {
        for (int i = collection_depth; i-- > 0;)
            if (collections[i].type == COLLECTION_APPLICATION)
                return collections[i].usage;
        return 0;
    }

    u32 physical() const {
        for (int i = collection_depth; i-- > 0;)
            if (collections[i].type == COLLECTION_PHYSICAL)
                return collections[i].usage;
        return 0;
    }

    long main_item(ReportKind kind, u32 flags) {
        u32& offset = bit_offset[static_cast<int>(kind)][g.report_id];
        u32 bits = g.report_count <= 4096 && g.report_size <= 256 ? g.report_size * g.report_count : 0;
        // A field we can't use (huge vendor blobs, too many fields or usages)
        // is skipped, but its bits still count, so the fields after it stay
        // where they are: one odd vendor section must not cost the device its
        // whole descriptor (wireless receivers then fall back to boot protocol,
        // which many ignore: 12-bit X/Y read as bytes made the pointer jump).
        auto skip = [&] {
            offset += bits;
            if (kind == ReportKind::Input && offset > out->max_input_bits)
                out->max_input_bits = offset;
            reset_locals();
            return 0L;
        };
        if (g.report_size == 0 || g.report_size > 32 || g.report_count > 4096 ||
            out->field_count >= ReportDescriptor::MAX_FIELDS)
            return skip();
        Field& f = out->fields[out->field_count];
        f.kind = kind;
        f.report_id = g.report_id;
        f.size = static_cast<u8>(g.report_size);
        f.count = static_cast<u16>(g.report_count);
        f.flags = flags;
        f.bit_offset = offset;
        f.logical_min = g.logical_min;
        // Logical Maximum is unsigned unless the minimum is negative (what devices mean, and what Linux does).
        f.logical_max = g.logical_min < 0 ? g.logical_max : static_cast<i32>(g.logical_max_raw);
        f.application = application();
        f.physical = physical();
        f.collection = collection_depth ? collections[collection_depth - 1].serial : 0;
        f.physical_min = g.physical_min;
        f.physical_max = g.physical_max;
        f.unit_exponent = g.unit_exponent;
        f.unit = g.unit;
        f.usage_first = static_cast<u16>(out->usage_count);
        f.usage_count = 0;
        f.usage_min = have_min ? resolve(usage_min) : 0;
        f.usage_max = have_max ? resolve(usage_max) : f.usage_min;
        if (have_min && have_max && f.usage_max < f.usage_min)
            return skip();

        // Explicit usage list; for variable fields a usage range expands into one too.
        int n = usage_count;
        if (n == 0 && have_min && (flags & FLAG_VARIABLE)) {
            for (u32 u = f.usage_min; u <= f.usage_max && n < f.count && n < 256; u++)
                usages[n++] = u;
        } else {
            for (int i = 0; i < n; i++)
                usages[i] = resolve(usages[i]);
        }
        if (out->usage_count + n > ReportDescriptor::MAX_USAGES)
            return skip();
        for (int i = 0; i < n; i++)
            out->usages[out->usage_count++] = usages[i];
        f.usage_count = static_cast<u16>(n);

        offset += bits;
        if (kind == ReportKind::Input && offset > out->max_input_bits)
            out->max_input_bits = offset;
        // Pure padding needn't be kept.
        if (!(flags & FLAG_CONSTANT) || n || have_min)
            out->field_count++;
        reset_locals();
        return 0;
    }
};

} // namespace

long parse(const u8* data, usize length, ReportDescriptor* out) {
    auto* p = static_cast<Parser*>(kmalloc(sizeof(Parser)));
    if (!p)
        return -err::NOMEM;
    new (p) Parser{};
    p->out = out;
    out->field_count = out->usage_count = out->application_count = 0;
    out->uses_report_ids = false;
    out->max_input_bits = 0;

    long result = 0;
    usize pos = 0;
    while (pos < length && result == 0) {
        u8 prefix = data[pos];
        if (prefix == 0xFE) { // long item: never used by real devices, skip it
            if (pos + 2 >= length) {
                result = -err::INVAL;
                break;
            }
            pos += 3 + data[pos + 1];
            continue;
        }
        u8 size = prefix & 3;
        if (size == 3)
            size = 4;
        u8 type = (prefix >> 2) & 3, tag = prefix >> 4;
        if (pos + 1 + size > length) {
            result = -err::INVAL;
            break;
        }
        u32 value = 0;
        for (u8 i = 0; i < size; i++)
            value |= static_cast<u32>(data[pos + 1 + i]) << (8 * i);
        i32 svalue = size == 1 ? static_cast<i8>(value) : size == 2 ? static_cast<i16>(value) : static_cast<i32>(value);
        pos += 1 + size;

        if (type == TYPE_MAIN) {
            switch (tag) {
            case MAIN_INPUT: result = p->main_item(ReportKind::Input, value); break;
            case MAIN_OUTPUT: result = p->main_item(ReportKind::Output, value); break;
            case MAIN_FEATURE: result = p->main_item(ReportKind::Feature, value); break;
            case MAIN_COLLECTION: {
                if (p->collection_depth >= 16) {
                    result = -err::INVAL;
                    break;
                }
                u32 u = p->usage_count ? p->resolve(p->usages[0]) : p->have_min ? p->resolve(p->usage_min) : 0;
                u8 ctype = static_cast<u8>(value);
                p->collections[p->collection_depth++] = Collection{ctype, u, ++p->collection_serial};
                if (ctype == COLLECTION_APPLICATION && p->collection_depth == 1 &&
                    out->application_count < ReportDescriptor::MAX_APPLICATIONS)
                    out->applications[out->application_count++] = u;
                p->reset_locals();
                break;
            }
            case MAIN_END_COLLECTION:
                if (p->collection_depth == 0)
                    result = -err::INVAL;
                else
                    p->collection_depth--;
                p->reset_locals();
                break;
            default:
                break;
            }
        } else if (type == TYPE_GLOBAL) {
            switch (tag) {
            case GLOBAL_USAGE_PAGE: p->g.usage_page = static_cast<u16>(value); break;
            case GLOBAL_LOGICAL_MIN: p->g.logical_min = svalue; break;
            case GLOBAL_LOGICAL_MAX:
                p->g.logical_max = svalue;
                p->g.logical_max_raw = value;
                p->g.logical_max_size = size;
                break;
            case GLOBAL_PHYSICAL_MIN: p->g.physical_min = svalue; break; // touchpads: size in mm
            case GLOBAL_PHYSICAL_MAX: p->g.physical_max = svalue; break;
            case GLOBAL_UNIT_EXPONENT: // 4-bit two's complement
                p->g.unit_exponent = static_cast<i8>(value & 0x8 ? static_cast<i32>(value & 0xF) - 16 : value & 0xF);
                break;
            case GLOBAL_UNIT: p->g.unit = value; break;
            case GLOBAL_REPORT_SIZE: p->g.report_size = value; break;
            case GLOBAL_REPORT_COUNT: p->g.report_count = value; break;
            case GLOBAL_REPORT_ID:
                if (value == 0 || value > 255)
                    result = -err::INVAL;
                p->g.report_id = static_cast<u8>(value);
                out->uses_report_ids = true;
                break;
            case GLOBAL_PUSH:
                if (p->stack_depth >= 4)
                    result = -err::INVAL;
                else
                    p->stack[p->stack_depth++] = p->g;
                break;
            case GLOBAL_POP:
                if (p->stack_depth == 0)
                    result = -err::INVAL;
                else
                    p->g = p->stack[--p->stack_depth];
                break;
            default:
                break;
            }
        } else if (type == TYPE_LOCAL) {
            u32 u = size == 4 ? value : (value & 0xFFFF) | IMPLICIT_PAGE;
            switch (tag) {
            case LOCAL_USAGE:
                if (p->usage_count < 256)
                    p->usages[p->usage_count++] = u;
                break;
            case LOCAL_USAGE_MIN: p->usage_min = u; p->have_min = true; break;
            case LOCAL_USAGE_MAX: p->usage_max = u; p->have_max = true; break;
            default: break; // designators, strings, delimiters
            }
        }
    }
    if (result == 0 && p->collection_depth != 0)
        result = -err::INVAL; // unbalanced collections
    kfree(p);
    return result;
}

u32 extract(const u8* report, u32 report_bytes, u32 bit_offset, u8 size) {
    u32 v = 0;
    for (u8 i = 0; i < size; i++) {
        u32 bit = bit_offset + i;
        if (bit / 8 >= report_bytes)
            break;
        v |= static_cast<u32>((report[bit / 8] >> (bit % 8)) & 1) << i;
    }
    return v;
}

// ---------------------------------------------------------------------------
// Usage -> evdev mapping
// ---------------------------------------------------------------------------

namespace {

// HID keyboard page -> Linux key code; same table as Linux's hid-input (hid_keyboard[]).
constexpr u8 KEYBOARD[256] = {
    0,   0,   0,   0,   30,  48,  46,  32,  18,  33,  34,  35,  23,  36,  37,  38,
    50,  49,  24,  25,  16,  19,  31,  20,  22,  47,  17,  45,  21,  44,  2,   3,
    4,   5,   6,   7,   8,   9,   10,  11,  28,  1,   14,  15,  57,  12,  13,  26,
    27,  43,  43,  39,  40,  41,  51,  52,  53,  58,  59,  60,  61,  62,  63,  64,
    65,  66,  67,  68,  87,  88,  99,  70,  119, 110, 102, 104, 111, 107, 109, 106,
    105, 108, 103, 69,  98,  55,  74,  78,  96,  79,  80,  81,  75,  76,  77,  71,
    72,  73,  82,  83,  86,  127, 116, 117, 183, 184, 185, 186, 187, 188, 189, 190,
    191, 192, 193, 194, 134, 138, 130, 132, 128, 129, 131, 137, 133, 135, 136, 113,
    115, 114, 0,   0,   0,   121, 0,   89,  93,  124, 92,  94,  95,  0,   0,   0,
    122, 123, 90,  91,  85,  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    29,  42,  56,  125, 97,  54,  100, 126, 164, 166, 165, 163, 161, 115, 114, 113,
    150, 158, 159, 128, 136, 177, 178, 176, 142, 152, 173, 140,
};

// Consumer page (media keys and friends) -> Linux key code.
struct ConsumerKey {
    u16 usage, key;
};
constexpr ConsumerKey CONSUMER[] = {
    {0x030, 116 /* POWER */},       {0x032, 142 /* SLEEP */},          {0x034, 142},
    {0x040, 139 /* MENU */},        {0x041, 353 /* SELECT */},         {0x06F, 225 /* BRIGHTNESSUP */},
    {0x070, 224 /* BRIGHTNESSDOWN */}, {0x0B0, 207 /* PLAY */},         {0x0B1, 119 /* PAUSE */},
    {0x0B2, 167 /* RECORD */},      {0x0B3, 208 /* FASTFORWARD */},    {0x0B4, 168 /* REWIND */},
    {0x0B5, 163 /* NEXTSONG */},    {0x0B6, 165 /* PREVIOUSSONG */},   {0x0B7, 166 /* STOPCD */},
    {0x0B8, 161 /* EJECTCD */},     {0x0CD, 164 /* PLAYPAUSE */},      {0x0E2, 113 /* MUTE */},
    {0x0E9, 115 /* VOLUMEUP */},    {0x0EA, 114 /* VOLUMEDOWN */},     {0x183, 226 /* MEDIA */},
    {0x18A, 155 /* MAIL */},        {0x192, 140 /* CALC */},           {0x194, 144 /* FILE */},
    {0x196, 150 /* WWW */},         {0x19E, 152 /* SCREENLOCK */},     {0x1A6, 138 /* HELP */},
    {0x201, 181 /* NEW */},         {0x202, 134 /* OPEN */},           {0x203, 206 /* CLOSE */},
    {0x207, 234 /* SAVE */},        {0x208, 210 /* PRINT */},          {0x21A, 131 /* UNDO */},
    {0x21B, 133 /* COPY */},        {0x21C, 137 /* CUT */},            {0x21D, 135 /* PASTE */},
    {0x221, 217 /* SEARCH */},      {0x223, 172 /* HOMEPAGE */},       {0x224, 158 /* BACK */},
    {0x225, 159 /* FORWARD */},     {0x226, 128 /* STOP */},           {0x227, 173 /* REFRESH */},
    {0x22A, 156 /* BOOKMARKS */},
};

u16 consumer_key(u16 id) {
    for (const ConsumerKey& c : CONSUMER)
        if (c.usage == id)
            return c.key;
    return 0;
}

// Hat switch direction -> (x, y), clockwise from north.
constexpr i8 HAT[9][2] = {{0, 0}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}};

i32 sign_extend(u32 v, u8 size) {
    if (size >= 32)
        return static_cast<i32>(v);
    u32 sign = 1u << (size - 1);
    return static_cast<i32>((v ^ sign) - sign);
}

} // namespace

u16 keyboard_usage_to_key(u8 usage_id) { return KEYBOARD[usage_id]; }

bool HidInput::map_usage(u32 u, const Field& f, Mapping* m) const {
    u16 page = static_cast<u16>(u >> 16), id = static_cast<u16>(u);
    *m = Mapping{};
    switch (page) {
    case PAGE_GENERIC_DESKTOP:
        if (id >= 0x30 && id <= 0x38) { // X Y Z Rx Ry Rz Slider Dial Wheel
            m->type = f.is_relative() ? input::EV_REL : input::EV_ABS;
            m->code = id & 0xF;
            return true;
        }
        if (id == 0x39) { // hat switch -> ABS_HAT0X/Y
            m->type = input::EV_ABS;
            m->code = input::ABS_HAT0X;
            m->hat = true;
            return true;
        }
        if (id >= 0x81 && id <= 0x83) { // system power down / sleep / wake up
            static constexpr u16 SYS[3] = {116, 142, 143};
            m->type = input::EV_KEY;
            m->code = SYS[id - 0x81];
            return true;
        }
        if (id >= 0x90 && id <= 0x93) { // D-pad up, down, right, left
            static constexpr u16 DPAD[4] = {input::BTN_DPAD_UP, input::BTN_DPAD_DOWN, input::BTN_DPAD_RIGHT,
                                            input::BTN_DPAD_LEFT};
            m->type = input::EV_KEY;
            m->code = DPAD[id - 0x90];
            return true;
        }
        return false;
    case PAGE_SIMULATION:
        m->type = input::EV_ABS;
        switch (id) {
        case 0xBA: m->code = input::ABS_RUDDER; return true;
        case 0xBB: m->code = input::ABS_THROTTLE; return true;
        case 0xC4: m->code = input::ABS_GAS; return true;
        case 0xC5: m->code = input::ABS_BRAKE; return true;
        case 0xC8: m->code = input::ABS_WHEEL; return true;
        default: return false;
        }
    case PAGE_KEYBOARD:
        if (id < 256 && KEYBOARD[id]) {
            m->type = input::EV_KEY;
            m->code = KEYBOARD[id];
            return true;
        }
        return false;
    case PAGE_BUTTON: {
        if (id == 0)
            return false;
        u16 n = static_cast<u16>(id - 1);
        u32 app = f.application;
        m->type = input::EV_KEY;
        if (app == GD_MOUSE || app == GD_POINTER || f.physical == GD_MOUSE || f.physical == GD_POINTER)
            m->code = static_cast<u16>(input::BTN_MOUSE + n);
        else if (app == GD_JOYSTICK)
            m->code = static_cast<u16>(n <= 0xF ? input::BTN_JOYSTICK + n : input::BTN_TRIGGER_HAPPY + n - 0x10);
        else if (app == GD_GAMEPAD)
            m->code = static_cast<u16>(n <= 0xF ? input::BTN_GAMEPAD + n : input::BTN_TRIGGER_HAPPY + n - 0x10);
        else if ((app >> 16) == PAGE_DIGITIZER)
            m->code = static_cast<u16>(n == 0 ? input::BTN_STYLUS : input::BTN_STYLUS2);
        else
            m->code = static_cast<u16>(n <= 0xF ? input::BTN_MISC + n : input::BTN_TRIGGER_HAPPY + n - 0x10);
        return m->code <= input::KEY_MAX;
    }
    case PAGE_CONSUMER:
        if (id == 0x238) { // AC Pan: horizontal wheel
            m->type = input::EV_REL;
            m->code = input::REL_HWHEEL;
            return true;
        }
        if (u16 key = consumer_key(id)) {
            m->type = input::EV_KEY;
            m->code = key;
            return true;
        }
        return false;
    case PAGE_DIGITIZER:
        switch (id) {
        case 0x30: m->type = input::EV_ABS; m->code = input::ABS_PRESSURE; return true;  // tip pressure
        case 0x32: m->type = input::EV_KEY; m->code = input::BTN_TOOL_PEN; return true;  // in range
        case 0x3D: m->type = input::EV_ABS; m->code = input::ABS_TILT_X; return true;
        case 0x3E: m->type = input::EV_ABS; m->code = input::ABS_TILT_Y; return true;
        case 0x42: m->type = input::EV_KEY; m->code = input::BTN_TOUCH; return true;     // tip switch
        case 0x44: m->type = input::EV_KEY; m->code = input::BTN_STYLUS; return true;    // barrel switch
        case 0x45: m->type = input::EV_KEY; m->code = 0x141; return true;                // eraser: BTN_TOOL_RUBBER
        default: return false;
        }
    default:
        return false;
    }
}

int HidInput::device_of(const Field& f) const {
    for (int i = 0; i < desc.application_count; i++)
        if (desc.applications[i] == f.application)
            return i;
    return 0;
}

void HidInput::declare(int dev, const Field& f, const Mapping& m) {
    input::Device& d = devices[dev];
    device_used[dev] = true;
    switch (m.type) {
    case input::EV_KEY:
        d.ev_bits |= 1ull << input::EV_KEY;
        d.set_key(m.code);
        break;
    case input::EV_REL:
        d.set_rel(m.code);
        // Every wheel event also comes as a high-resolution one (120 units per
        // notch), like Linux, so smooth-scrolling clients find REL_WHEEL_HI_RES.
        if (m.code == input::REL_WHEEL)
            d.set_rel(input::REL_WHEEL_HI_RES);
        if (m.code == input::REL_HWHEEL)
            d.set_rel(input::REL_HWHEEL_HI_RES);
        break;
    case input::EV_ABS:
        if (m.hat) {
            d.set_abs(input::ABS_HAT0X, -1, 1);
            d.set_abs(input::ABS_HAT0Y, -1, 1);
        } else {
            i32 lo = f.logical_min, hi = f.logical_max;
            // Joysticks and gamepads get a dead zone in the middle (Linux: range / 16).
            bool stick = f.application == GD_JOYSTICK || f.application == GD_GAMEPAD;
            d.set_abs(m.code, lo, hi, stick ? (hi - lo) >> 4 : 0);
            d.abs_value[m.code] = stick && (m.code <= input::ABS_RZ) && lo < hi ? lo + (hi - lo) / 2 : 0;
        }
        break;
    default:
        break;
    }
}

int HidInput::init(const u8* descriptor, usize length, EventSink* sink) {
    m_sink = sink;
    if (parse(descriptor, length, &desc) != 0)
        return 0;
    if (desc.application_count == 0) { // no application collection: treat as one device
        desc.applications[0] = 0;
        desc.application_count = 1;
    }
    device_count = desc.application_count;

    for (int fi = 0; fi < desc.field_count; fi++) {
        Field& f = desc.fields[fi];
        if (f.kind != ReportKind::Input || f.is_constant())
            continue;
        int dev = device_of(f);
        Mapping m;
        if (f.is_variable()) {
            for (u16 i = 0; i < f.usage_count; i++)
                if (map_usage(desc.usages[f.usage_first + i], f, &m))
                    declare(dev, f, m);
        } else if (f.usage_count) {
            for (u16 i = 0; i < f.usage_count; i++)
                if (map_usage(desc.usages[f.usage_first + i], f, &m))
                    declare(dev, f, m);
        } else {
            for (u32 u = f.usage_min; u <= f.usage_max && u - f.usage_min < 1024; u++)
                if (map_usage(u, f, &m))
                    declare(dev, f, m);
        }
        m_state[fi] = static_cast<i32*>(kmalloc(sizeof(i32) * (f.count ? f.count : 1)));
        if (m_state[fi]) {
            for (u16 i = 0; i < f.count; i++)
                m_state[fi][i] = 0;
        }
    }

    int used = 0;
    for (int i = 0; i < device_count; i++) {
        u32 app = desc.applications[i];
        is_keyboard[i] = app == GD_KEYBOARD || app == GD_KEYPAD;
        if (devices[i].ev_bits & (1ull << input::EV_KEY)) {
            // Keyboards repeat keys (EV_REP), like Linux sets for them.
            if (is_keyboard[i])
                devices[i].ev_bits |= 1ull << input::EV_REP;
        }
        if (app == DG_PEN || app == DG_DIGITIZER)
            devices[i].props |= 1u << input::INPUT_PROP_POINTER;
        if (app == DG_TOUCHSCREEN)
            devices[i].props |= 1u << input::INPUT_PROP_DIRECT;
        used += device_used[i];
    }
    return used;
}

u32 HidInput::max_report_bytes() const {
    return (desc.max_input_bits + 7) / 8 + (desc.uses_report_ids ? 1 : 0);
}

void HidInput::emit(int dev, u16 type, u16 code, i32 value) {
    m_dirty[dev] = true;
    m_sink->event(dev, type, code, value);
}

void HidInput::process_variable(int fi, const u8* data, u32 bytes) {
    const Field& f = desc.fields[fi];
    int dev = device_of(f);
    bool is_signed = f.logical_min < 0;
    for (u16 i = 0; i < f.count && f.usage_count; i++) {
        u32 u = desc.usages[f.usage_first + (i < f.usage_count ? i : f.usage_count - 1)];
        Mapping m;
        if (!map_usage(u, f, &m))
            continue;
        u32 raw = extract(data, bytes, f.bit_offset + i * f.size, f.size);
        i32 v = is_signed ? sign_extend(raw, f.size) : static_cast<i32>(raw);
        i32& prev = m_state[fi][i];
        bool in_range = v >= f.logical_min && v <= f.logical_max;

        if (m.hat) {
            // Directions spread over the logical range (8 or 4 positions); anything outside is "centered".
            int dir = 0;
            if (in_range && f.logical_max > f.logical_min)
                dir = static_cast<int>((v - f.logical_min) * 8 / (f.logical_max - f.logical_min + 1)) + 1;
            if (dir != prev) {
                emit(dev, input::EV_ABS, input::ABS_HAT0X, HAT[dir][0]);
                emit(dev, input::EV_ABS, input::ABS_HAT0Y, HAT[dir][1]);
                prev = dir;
            }
            continue;
        }
        switch (m.type) {
        case input::EV_REL:
            if (v) {
                emit(dev, input::EV_REL, m.code, v);
                if (m.code == input::REL_WHEEL)
                    emit(dev, input::EV_REL, input::REL_WHEEL_HI_RES, v * 120);
                else if (m.code == input::REL_HWHEEL)
                    emit(dev, input::EV_REL, input::REL_HWHEEL_HI_RES, v * 120);
            }
            break;
        case input::EV_KEY: {
            i32 down = v != 0 && in_range ? 1 : 0;
            if (down != prev) {
                emit(dev, input::EV_KEY, m.code, down);
                prev = down;
            }
            break;
        }
        case input::EV_ABS:
            if ((f.flags & FLAG_NULL_STATE) && !in_range)
                break; // "no value" (e.g. pressure while the pen is away)
            if (v != prev) {
                emit(dev, input::EV_ABS, m.code, v);
                prev = v;
            }
            break;
        default:
            break;
        }
    }
}

u32 HidInput::array_usage(const Field& f, i32 value) const {
    if (value < f.logical_min || value > f.logical_max)
        return 0;
    u32 index = static_cast<u32>(value - f.logical_min);
    if (f.usage_count)
        return index < f.usage_count ? desc.usages[f.usage_first + index] : 0;
    u32 u = f.usage_min + index;
    return u <= f.usage_max ? u : 0;
}

void HidInput::process_array(int fi, const u8* data, u32 bytes) {
    const Field& f = desc.fields[fi];
    int dev = device_of(f);
    bool is_signed = f.logical_min < 0;
    constexpr int MAX = 64;
    u32 now[MAX];
    int n = f.count < MAX ? f.count : MAX;
    for (int i = 0; i < n; i++) {
        u32 raw = extract(data, bytes, f.bit_offset + i * f.size, f.size);
        now[i] = array_usage(f, is_signed ? sign_extend(raw, f.size) : static_cast<i32>(raw));
        // Keyboard "ErrorRollOver"/"POSTFail"/"ErrorUndefined": too many keys at once,
        // the report says nothing reliable - keep the previous state.
        if (now[i] >= usage(PAGE_KEYBOARD, 1) && now[i] <= usage(PAGE_KEYBOARD, 3))
            return;
    }
    auto contains = [](const u32* set, int count, u32 u) {
        for (int i = 0; i < count; i++)
            if (static_cast<u32>(set[i]) == u)
                return true;
        return false;
    };
    u32 before[MAX];
    for (int i = 0; i < n; i++)
        before[i] = static_cast<u32>(m_state[fi][i]);
    Mapping m;
    for (int i = 0; i < n; i++) // released
        if (before[i] && !contains(now, n, before[i]) && map_usage(before[i], f, &m) && m.type == input::EV_KEY)
            emit(dev, input::EV_KEY, m.code, 0);
    for (int i = 0; i < n; i++) // pressed
        if (now[i] && !contains(before, n, now[i]) && map_usage(now[i], f, &m) && m.type == input::EV_KEY)
            emit(dev, input::EV_KEY, m.code, 1);
    for (int i = 0; i < n; i++)
        m_state[fi][i] = static_cast<i32>(now[i]);
}

void HidInput::process(const u8* report, u32 length) {
    u8 id = 0;
    if (desc.uses_report_ids) {
        if (length < 1)
            return;
        id = report[0];
        report++;
        length--;
    }
    for (int fi = 0; fi < desc.field_count; fi++) {
        const Field& f = desc.fields[fi];
        if (f.kind != ReportKind::Input || f.report_id != id || f.is_constant() || !m_state[fi])
            continue;
        if ((f.bit_offset + static_cast<u32>(f.size) * f.count + 7) / 8 > length)
            continue; // short report
        if (f.is_variable())
            process_variable(fi, report, length);
        else
            process_array(fi, report, length);
    }
    for (int i = 0; i < device_count; i++) {
        if (m_dirty[i]) {
            m_sink->event(i, input::EV_SYN, input::SYN_REPORT, 0);
            m_dirty[i] = false;
        }
    }
}

HidInput::~HidInput() {
    for (int i = 0; i < ReportDescriptor::MAX_FIELDS; i++)
        kfree(m_state[i]);
}

} // namespace usb::hid
