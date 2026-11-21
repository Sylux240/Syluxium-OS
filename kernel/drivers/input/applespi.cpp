// Apple SPI keyboard and trackpad (see applespi.hpp).
//
// Exchanges: a GPE means one 256-byte packet is waiting and is read. A
// command is a written packet followed at once by a 4-byte status read
// (ac 27 68 d5 = accepted); its answer arrives later like any read, with
// the write flag set. Messages larger than one packet's 246 data bytes
// (many fingers) span two packets. Every packet and every message ends with
// a CRC-16 (ARC: polynomial 0x8005 reflected, initial 0).

#include <acpi/devices.hpp>
#include <core/log.hpp>
#include <drivers/input/applespi.hpp>
#include <drivers/input/input.hpp>
#include <drivers/input/touchpad.hpp>
#include <drivers/pci/pci.hpp>
#include <drivers/spi/lpss_spi.hpp>
#include <drivers/usb/hid_parser.hpp>
#include <lib/errno.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/vmalloc.hpp>
#include <sched/sched.hpp>
#include <sched/sync.hpp>
#include <time/clock.hpp>

#include <uacpi/event.h>
#include <uacpi/namespace.h>
#include <uacpi/resources.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

namespace applespi {

namespace {

// ---- protocol ----

constexpr u32 PACKET_SIZE = 256, PACKET_DATA = 246, STATUS_SIZE = 4, MSG_HEADER = 8, MAX_MSG = 2 * PACKET_SIZE;
constexpr u8 PKT_READ = 0x20, PKT_WRITE = 0x40;
constexpr u8 DEV_KEYBOARD = 0x01, DEV_TOUCHPAD = 0x02, DEV_INFO = 0xD0;
constexpr u16 MSG_KEYBOARD = 0x0110, MSG_TOUCHPAD = 0x0210, MSG_TP_INFO = 0x1020, MSG_MT_INIT = 0x0252;
constexpr int ROLLOVER = 6, MAX_FINGERS = 11;
constexpr u32 TP_HEADER = 48, FINGER_SIZE = 30; // touchpad message: header, then 15 le16 per finger
constexpr u8 STATUS_OK[STATUS_SIZE] = {0xAC, 0x27, 0x68, 0xD5};
constexpr u32 RW_CHANGE_DELAY_US = 100; // between a write and its status read (Linux: found by experiment)

u16 crc16(const u8* p, u32 n, u16 crc = 0) {
    for (u32 i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? static_cast<u16>((crc >> 1) ^ 0xA001) : static_cast<u16>(crc >> 1);
    }
    return crc;
}
u16 le16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
i16 le16s(const u8* p) { return static_cast<i16>(le16(p)); }
void put16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
}

// A message: header (type, 0, counter, response length, length) + payload
// + CRC. `payload_len` excludes the CRC. Returns the whole length.
u32 build_message(u8* m, u16 type, u8 zero, u8 counter, u16 rsp_len, const u8* payload, u32 payload_len) {
    put16(m, type);
    m[2] = zero;
    m[3] = counter;
    put16(m + 6, static_cast<u16>(payload_len));
    put16(m + 4, rsp_len ? rsp_len : static_cast<u16>(payload_len));
    if (payload_len)
        memcpy(m + MSG_HEADER, payload, payload_len);
    put16(m + MSG_HEADER + payload_len, crc16(m, MSG_HEADER + payload_len));
    return MSG_HEADER + payload_len + 2;
}

// Split a message over packets (at most 2). Returns the packet count.
int build_packets(u8 (*pkts)[PACKET_SIZE], u8 flags, u8 device, const u8* msg, u32 msg_len) {
    int n = 0;
    for (u32 off = 0; off < msg_len && n < 2; n++) {
        u8* p = pkts[n];
        memset(p, 0, PACKET_SIZE);
        u32 len = msg_len - off > PACKET_DATA ? PACKET_DATA : msg_len - off;
        p[0] = flags;
        p[1] = device;
        put16(p + 2, static_cast<u16>(off));
        put16(p + 4, static_cast<u16>(msg_len - off - len));
        put16(p + 6, static_cast<u16>(len));
        memcpy(p + 8, msg + off, len);
        put16(p + PACKET_SIZE - 2, crc16(p, PACKET_SIZE - 2));
        off += len;
    }
    return n;
}

// Reassembles messages from packets.
struct Assembler {
    u8 buf[MAX_MSG];
    u32 saved = 0;
    const char* error = nullptr;

    // >0: a complete, checked message of that length at *msg; 0: more
    // packets needed; <0: a bad packet (error says why).
    int feed(const u8* pkt, const u8** msg) {
        if (crc16(pkt, PACKET_SIZE) != 0)
            return fail("packet CRC mismatch");
        u32 off = le16(pkt + 2), rem = le16(pkt + 4), len = le16(pkt + 6);
        if (len > PACKET_DATA)
            return fail("packet length");
        u32 mlen;
        if (rem || off) {
            if (off != saved || off + rem > MAX_MSG || off + len > MAX_MSG)
                return fail("unexpected packet offset");
            memcpy(buf + off, pkt + 8, len);
            saved += len;
            if (rem)
                return 0;
            *msg = buf;
            mlen = saved;
            saved = 0;
        } else {
            *msg = pkt + 8;
            mlen = len;
        }
        if (mlen < MSG_HEADER + 2)
            return fail("message too short");
        if (crc16(*msg, mlen) != 0)
            return fail("message CRC mismatch");
        if (le16(*msg + 6) != mlen - MSG_HEADER - 2)
            return fail("message length");
        return static_cast<int>(mlen);
    }
    int fail(const char* why) {
        saved = 0;
        error = why;
        return -1;
    }
};

struct KeyboardReport {
    u8 modifiers;
    u8 keys[ROLLOVER];
    bool fn;
};

bool decode_keyboard(const u8* msg, u32 len, KeyboardReport* k) {
    if (len < MSG_HEADER + 10 + 2)
        return false;
    const u8* p = msg + MSG_HEADER;
    k->modifiers = p[1];
    memcpy(k->keys, p + 3, ROLLOVER);
    k->fn = p[9] != 0;
    return true;
}

struct TouchFrame {
    bool clicked;
    int count;
    input::TouchpadMouse::Finger fingers[MAX_FINGERS];
};

bool decode_touchpad(const u8* msg, u32 len, TouchFrame* t) {
    const u8* p = msg + MSG_HEADER;
    if (len < MSG_HEADER + TP_HEADER + 2)
        return false;
    u32 n = p[30];
    if (le16(msg + 6) + 2u != TP_HEADER + n * FINGER_SIZE)
        return false;
    if (n > MAX_FINGERS)
        n = MAX_FINGERS;
    t->clicked = p[1] != 0;
    t->count = 0;
    for (u32 i = 0; i < n; i++) {
        const u8* f = p + TP_HEADER + i * FINGER_SIZE;
        if (le16s(f + 16) == 0) // touch_major: lifted
            continue;
        // Apple's y grows towards the keyboard (upwards), as TouchpadMouse wants.
        t->fingers[t->count++] = {le16s(f + 2), le16s(f + 4)};
    }
    return true;
}

// Linux key codes.
constexpr u16 KEY_ENTER = 28, KEY_BACKSPACE = 14, KEY_F1 = 59, KEY_F2 = 60, KEY_F3 = 61, KEY_F4 = 62, KEY_F5 = 63,
              KEY_F6 = 64, KEY_F7 = 65, KEY_F8 = 66, KEY_F9 = 67, KEY_F10 = 68, KEY_F11 = 87, KEY_F12 = 88,
              KEY_RIGHT = 106, KEY_LEFT = 105, KEY_DOWN = 108, KEY_UP = 103, KEY_DELETE = 111, KEY_INSERT = 110,
              KEY_HOME = 102, KEY_END = 107, KEY_PAGEUP = 104, KEY_PAGEDOWN = 109, KEY_BRIGHTNESSDOWN = 224,
              KEY_BRIGHTNESSUP = 225, KEY_SCALE = 120, KEY_DASHBOARD = 204, KEY_KBDILLUMDOWN = 229,
              KEY_KBDILLUMUP = 230, KEY_FN = 0x1D0;
// The modifier byte's bits (bit 4 unused).
constexpr u16 MODIFIER_KEYS[8] = {29 /* LEFTCTRL */, 42 /* LEFTSHIFT */, 56 /* LEFTALT */, 125 /* LEFTMETA */,
                                  0,  54 /* RIGHTSHIFT */, 100 /* RIGHTALT */, 126 /* RIGHTMETA */};

// Fn combinations. The top row gives the printed functions (brightness,
// media, volume) unless Fn is held, as on macOS (Linux: fnmode=1).
struct FnKey {
    u16 from, to;
    bool top_row;
};
constexpr FnKey FN_KEYS[] = {
    {KEY_BACKSPACE, KEY_DELETE, false},     {KEY_ENTER, KEY_INSERT, false},
    {KEY_F1, KEY_BRIGHTNESSDOWN, true},     {KEY_F2, KEY_BRIGHTNESSUP, true},
    {KEY_F3, KEY_SCALE, true},              {KEY_F4, KEY_DASHBOARD, true},
    {KEY_F5, KEY_KBDILLUMDOWN, true},       {KEY_F6, KEY_KBDILLUMUP, true},
    {KEY_F7, input::KEY_PREVIOUSSONG, true}, {KEY_F8, input::KEY_PLAYPAUSE, true},
    {KEY_F9, input::KEY_NEXTSONG, true},    {KEY_F10, input::KEY_MUTE, true},
    {KEY_F11, input::KEY_VOLUMEDOWN, true}, {KEY_F12, input::KEY_VOLUMEUP, true},
    {KEY_RIGHT, KEY_END, false},            {KEY_LEFT, KEY_HOME, false},
    {KEY_DOWN, KEY_PAGEDOWN, false},        {KEY_UP, KEY_PAGEUP, false},
};

// The keyboard reports HID keyboard-page usages.
u16 code_to_key(u8 code, bool fn) {
    if (code < 4)
        return 0;
    u16 key = usb::hid::keyboard_usage_to_key(code);
    for (const FnKey& t : FN_KEYS)
        if (t.from == key) {
            if (t.top_row ? !fn : fn)
                key = t.to;
            break;
        }
    return key;
}

// Trackpad sizes by model number (Linux's table): x_min, y_min, x_max, y_max.
struct PadModel {
    u8 model;
    i32 x_min, y_min, x_max, y_max;
};
constexpr PadModel PAD_MODELS[] = {
    {0x04, -5087, -182, 5579, 6089}, // MacBook8,1 9,1 10,1
    {0x05, -6243, -170, 6749, 7685}, // MacBookPro13,1 13,2 14,1 14,2
    {0x06, -7456, -163, 7976, 9283}, // MacBookPro13,3 14,3
};

// ---- the device ----

class AppleSpi {
public:
    uacpi_namespace_node* node = nullptr;
    spi::LpssSpi bus;
    spi::Mode mode;
    u32 cs_delay_us = 10;
    u16 gpe = 0;
    sched::Semaphore data_ready;
    u8 tx[PACKET_SIZE], rx[PACKET_SIZE], status[STATUS_SIZE];
    u8 counter = 0;
    Assembler asm_;
    input::Device keyboard;
    bool keyboard_up = false;
    input::TouchpadMouse pad;
    bool pad_up = false;
    u16 last_keys[ROLLOVER] = {};
    u8 last_modifiers = 0;
    bool last_fn = false;
    u32 bad_packets = 0;

    long read_packet() {
        LockGuard g(bus.lock());
        bus.cs(true);
        clock::busy_wait_ns(cs_delay_us * 1000ull);
        long e = bus.transfer(mode, nullptr, rx, PACKET_SIZE);
        bus.cs(false);
        return e;
    }

    // Send one command; true if the controller accepted it.
    bool command(u8 device, u16 type, u8 zero, u16 rsp_len, const u8* payload, u32 payload_len) {
        u8 msg[64];
        u32 mlen = build_message(msg, type, zero, counter++, rsp_len, payload, payload_len);
        u8 pkts[2][PACKET_SIZE];
        build_packets(pkts, PKT_WRITE, device, msg, mlen);
        memcpy(tx, pkts[0], PACKET_SIZE);
        LockGuard g(bus.lock());
        clock::busy_wait_ns(RW_CHANGE_DELAY_US * 1000ull);
        bus.cs(true);
        clock::busy_wait_ns(cs_delay_us * 1000ull);
        long e = bus.transfer(mode, tx, nullptr, PACKET_SIZE);
        clock::busy_wait_ns(RW_CHANGE_DELAY_US * 1000ull);
        if (e == 0)
            e = bus.transfer(mode, nullptr, status, STATUS_SIZE);
        bus.cs(false);
        if (e < 0) {
            klog::warn("applespi: command %#x: SPI transfer failed (%ld)", type, e);
            return false;
        }
        if (memcmp(status, STATUS_OK, STATUS_SIZE) != 0) {
            klog::warn("applespi: command %#x not accepted (status %02x %02x %02x %02x)", type, status[0], status[1],
                       status[2], status[3]);
            return false;
        }
        return true;
    }

    bool request_pad_info() { return command(DEV_INFO, MSG_TP_INFO, 0x02, 0x0200, nullptr, 0); }
    bool request_multitouch() {
        const u8 cmd[2] = {0x02, 0x01}; // 0x0102
        return command(DEV_TOUCHPAD, MSG_MT_INIT, 0, 0, cmd, sizeof(cmd));
    }

    void register_keyboard() {
        snprintf(keyboard.name, sizeof(keyboard.name), "Apple SPI Keyboard");
        keyboard.bustype = 0x1C; // BUS_SPI
        keyboard.vendor = 0x05AC;
        keyboard.ev_bits = (1ull << input::EV_KEY) | (1ull << input::EV_REP);
        for (u16 u = 4; u < 0xE8; u++)
            if (u16 k = usb::hid::keyboard_usage_to_key(static_cast<u8>(u)))
                keyboard.set_key(k);
        for (u16 k : MODIFIER_KEYS)
            if (k)
                keyboard.set_key(k);
        for (const FnKey& t : FN_KEYS)
            keyboard.set_key(t.to);
        keyboard.set_key(KEY_FN);
        input::register_device(&keyboard);
        keyboard_up = true;
    }

    void register_pad(u8 model_no, u8 model_flags) {
        const PadModel* m = &PAD_MODELS[0];
        bool known = false;
        for (const PadModel& p : PAD_MODELS)
            if (p.model == model_no) {
                m = &p;
                known = true;
            }
        if (!known)
            klog::warn("applespi: unknown trackpad model %#x: using the 12-inch MacBook's size", model_no);
        // ~100 units per mm on every model.
        pad.init("Apple SPI Touchpad", 0x1C /* BUS_SPI */, 0x06CB, static_cast<u16>(model_no << 8 | model_flags),
                 100);
        pad.set_area(m->x_min, m->x_max, m->y_min, m->y_max, false);
        pad_up = true;
        klog::ok("applespi: trackpad model %#x (%d x %d units)", model_no, m->x_max - m->x_min, m->y_max - m->y_min);
    }

    void keyboard_message(const KeyboardReport& r) {
        if (!keyboard_up)
            return;
        bool overflow = true; // too many keys: all positions read 1
        for (u8 k : r.keys)
            overflow &= k == 1;
        if (overflow)
            return;
        u16 now[ROLLOVER];
        for (int i = 0; i < ROLLOVER; i++)
            now[i] = code_to_key(r.keys[i], r.fn);
        // Released: keys of the last report no longer down (as they were
        // translated then, so Fn let go first still releases the right key).
        for (int i = 0; i < ROLLOVER; i++) {
            if (!last_keys[i])
                continue;
            bool held = false;
            for (int j = 0; j < ROLLOVER; j++)
                held |= r.keys[j] && last_keys[i] == code_to_key(r.keys[j], last_fn);
            if (!held)
                keyboard.report(input::EV_KEY, last_keys[i], 0);
        }
        for (int i = 0; i < ROLLOVER; i++) {
            bool was = false;
            for (int j = 0; j < ROLLOVER; j++)
                was |= now[i] && last_keys[j] == now[i];
            if (now[i] && !was)
                keyboard.report(input::EV_KEY, now[i], 1);
        }
        for (int b = 0; b < 8; b++)
            if (MODIFIER_KEYS[b] && ((r.modifiers ^ last_modifiers) & (1 << b)))
                keyboard.report(input::EV_KEY, MODIFIER_KEYS[b], (r.modifiers >> b) & 1);
        if (r.fn != last_fn)
            keyboard.report(input::EV_KEY, KEY_FN, r.fn);
        keyboard.sync();
        memcpy(last_keys, now, sizeof(now));
        last_modifiers = r.modifiers;
        last_fn = r.fn;
    }

    // One packet read after a GPE. Returns the answered command type, if any.
    u16 handle_packet() {
        const u8* msg = nullptr;
        int len = asm_.feed(rx, &msg);
        if (len < 0) {
            if (++bad_packets <= 10)
                klog::warn("applespi: bad packet (%s)", asm_.error);
            return 0;
        }
        if (len == 0)
            return 0;
        u16 type = le16(msg);
        u8 flags = rx[0], device = rx[1];
        if (flags == PKT_READ && device == DEV_KEYBOARD) {
            KeyboardReport r;
            if (decode_keyboard(msg, static_cast<u32>(len), &r))
                keyboard_message(r);
        } else if (flags == PKT_READ && device == DEV_TOUCHPAD) {
            TouchFrame t;
            if (pad_up && decode_touchpad(msg, static_cast<u32>(len), &t))
                pad.frame(t.fingers, t.count, t.clicked);
        } else if (flags == PKT_WRITE) {
            if (device == DEV_INFO && type == MSG_TP_INFO && len >= static_cast<int>(MSG_HEADER + 107)) {
                if (!pad_up)
                    register_pad(msg[MSG_HEADER + 106], msg[MSG_HEADER + 105]);
            } else if (device == DEV_TOUCHPAD && type == MSG_MT_INIT) {
                klog::info("applespi: trackpad in multi-touch mode");
            }
            return type;
        }
        return 0;
    }

    // Wait for a GPE (up to `ms`) and handle what it announced.
    u16 wait_and_handle(u32 ms) {
        if (!data_ready.wait_timeout(ms * 1'000'000ull))
            return 0xFFFF;
        long e = read_packet();
        uacpi_finish_handling_gpe(nullptr, gpe); // level-triggered: masked until now
        if (e < 0) {
            klog::warn("applespi: reading a packet failed (%ld)", e);
            return 0;
        }
        return handle_packet();
    }

    // Send a command and wait for its answer (a write exchange). Other
    // messages arriving meanwhile are handled.
    bool exchange(bool (AppleSpi::*send)(), u16 type) {
        for (int attempt = 0; attempt < 3; attempt++) {
            if (!(this->*send)()) {
                sched::sleep_ms(100);
                continue;
            }
            u64 deadline = clock::now_ns() + 1'000'000'000;
            while (clock::now_ns() < deadline) {
                u16 r = wait_and_handle(100);
                if (r == type)
                    return true;
            }
            klog::warn("applespi: no answer to command %#x", type);
        }
        return false;
    }
};

uacpi_interrupt_ret gpe_handler(uacpi_handle ctx, uacpi_namespace_node*, uacpi_u16) {
    static_cast<AppleSpi*>(ctx)->data_ready.signal();
    return UACPI_INTERRUPT_HANDLED; // re-enabled once the packet is read
}

// ---- ACPI: properties, controller ----

// Apple's device properties: _DSM {a0b5b7c6-1318-441c-b0c9-fe695eaf949b}
// function 1 returns a package of name/value pairs (function 0: version 3).
struct Properties {
    u64 sclk_period_ns = 0, word_size = 8, bit_order = 1, spo = 0, sph = 0, cs_delay_us = 10;
    bool found = false;
};

uacpi_object* apple_dsm(uacpi_namespace_node* node, u64 function) {
    static const u8 uuid[16] = {0xC6, 0xB7, 0xB5, 0xA0, 0x18, 0x13, 0x1C, 0x44,
                                0xB0, 0xC9, 0xFE, 0x69, 0x5E, 0xAF, 0x94, 0x9B};
    uacpi_data_view view;
    view.const_bytes = uuid;
    view.length = sizeof(uuid);
    uacpi_object* args[4] = {uacpi_object_create_buffer(view), uacpi_object_create_integer(1),
                             uacpi_object_create_integer(function), uacpi_object_create_package({nullptr, 0})};
    uacpi_object_array arr{args, 4};
    uacpi_object* ret = nullptr;
    if (!args[0] || !args[1] || !args[2] || !args[3] || uacpi_eval(node, "_DSM", &arr, &ret) != UACPI_STATUS_OK)
        ret = nullptr;
    for (auto* a : args)
        if (a)
            uacpi_object_unref(a);
    return ret;
}

u64 property_value(uacpi_object* v) {
    u64 x = 0;
    if (uacpi_object_get_type(v) == UACPI_OBJECT_INTEGER) {
        uacpi_object_get_integer(v, &x);
        return x;
    }
    uacpi_data_view d;
    if (uacpi_object_get_type(v) == UACPI_OBJECT_BUFFER && uacpi_object_get_string_or_buffer(v, &d) == UACPI_STATUS_OK)
        for (usize i = 0; i < d.length && i < 8; i++)
            x |= static_cast<u64>(d.bytes[i]) << (8 * i);
    return x;
}

Properties read_properties(uacpi_namespace_node* node) {
    Properties p;
    uacpi_object* v = apple_dsm(node, 0);
    uacpi_data_view d;
    bool ok = v && uacpi_object_get_type(v) == UACPI_OBJECT_BUFFER &&
              uacpi_object_get_string_or_buffer(v, &d) == UACPI_STATUS_OK && d.length && d.bytes[0] == 3;
    if (v)
        uacpi_object_unref(v);
    if (!ok)
        return p;
    uacpi_object* pkg = apple_dsm(node, 1);
    uacpi_object_array a;
    if (!pkg || uacpi_object_get_package(pkg, &a) != UACPI_STATUS_OK) {
        if (pkg)
            uacpi_object_unref(pkg);
        return p;
    }
    p.found = true;
    for (usize i = 0; i + 1 < a.count; i += 2) {
        uacpi_data_view key;
        if (uacpi_object_get_type(a.objects[i]) != UACPI_OBJECT_STRING ||
            uacpi_object_get_string(a.objects[i], &key) != UACPI_STATUS_OK)
            continue;
        u64 val = property_value(a.objects[i + 1]);
        auto is = [&](const char* name) { return !strcmp(key.text, name); };
        if (is("spiSclkPeriod"))
            p.sclk_period_ns = val & 0xFFFFFFFF;
        else if (is("spiWordSize"))
            p.word_size = val;
        else if (is("spiBitOrder"))
            p.bit_order = val;
        else if (is("spiSPO"))
            p.spo = val;
        else if (is("spiSPH"))
            p.sph = val;
        else if (is("spiCSDelay"))
            p.cs_delay_us = val;
    }
    uacpi_object_unref(pkg);
    return p;
}

struct MemSearch {
    u64 base = 0;
    u32 length = 0;
};

uacpi_iteration_decision find_memory(void* user, uacpi_resource* r) {
    auto* m = static_cast<MemSearch*>(user);
    if (r->type == UACPI_RESOURCE_TYPE_FIXED_MEMORY32 && !m->base) {
        m->base = r->fixed_memory32.address;
        m->length = r->fixed_memory32.length;
    } else if (r->type == UACPI_RESOURCE_TYPE_MEMORY32 && !m->base) {
        m->base = r->memory32.minimum;
        m->length = r->memory32.length;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

// The SPI controller is the APP000D device's parent: a PCI function
// (Sunrise Point and later, found by _ADR) or an ACPI device (Lynx/Wildcat
// Point INT33C0/INT33C1/INT3430/INT3431, registers in _CRS).
bool setup_controller(AppleSpi* d, uacpi_namespace_node* ctrl, char* name, usize name_size) {
    const char* abs = uacpi_namespace_node_generate_absolute_path(ctrl);
    snprintf(name, name_size, "%s", abs ? abs : "?");
    if (abs)
        uacpi_free_absolute_path(abs);
    uacpi_execute_simple(ctrl, "_PS0");
    u64 adr = 0;
    pci::Function* pf = nullptr;
    if (uacpi_eval_adr(ctrl, &adr) == UACPI_STATUS_OK) {
        u64 bbn = 0;
        if (uacpi_namespace_node* parent = uacpi_namespace_node_parent(ctrl))
            uacpi_eval_simple_integer(parent, "_BBN", &bbn);
        u8 dev = static_cast<u8>(adr >> 16), fn = static_cast<u8>(adr & 0xFFFF);
        for (u32 i = 0; i < pci::count(); i++) {
            pci::Function* f = pci::function_at(i);
            if (f->segment == 0 && f->bus == bbn && f->device == dev && f->function == fn)
                pf = f;
        }
    }
    if (pf) {
        if (pf->vendor_id != 0x8086) {
            klog::warn("applespi: SPI controller %s is %04x:%04x, not an Intel LPSS port", name, pf->vendor_id,
                       pf->device_id);
            return false;
        }
        for (u8 cap = pci::read8(*pf, 0x34) & 0xFC, n = 0; cap && n < 48; cap = pci::read8(*pf, cap + 1) & 0xFC, n++)
            if (pci::read8(*pf, cap) == 0x01) { // power management: D0
                u16 pmcsr = pci::read16(*pf, cap + 4);
                if (pmcsr & 3) {
                    pci::write16(*pf, cap + 4, pmcsr & ~3);
                    sched::sleep_ms(10);
                }
            }
        pci::enable(*pf);
        auto* regs = static_cast<volatile u8*>(pci::map_bar(*pf, 0));
        if (!regs || pf->bars[0].size < 0x400) {
            klog::warn("applespi: SPI controller %s: no usable register BAR", name);
            return false;
        }
        // Broxton/Apollo Lake LPSS runs at 100 MHz; Sunrise Point (the
        // Skylake/Kaby Lake Macs) and Cannon Lake at 120 MHz.
        u32 clock = (pf->device_id & 0xFF00) == 0x5A00 ? 100000000 : 120000000;
        return d->bus.init(regs, spi::LpssGen::SunrisePoint, clock, name);
    }
    MemSearch m;
    uacpi_for_each_device_resource(ctrl, "_CRS", find_memory, &m);
    if (!m.base || m.length < 0x1000) {
        klog::warn("applespi: SPI controller %s: registers not found (no PCI function, no _CRS memory)", name);
        return false;
    }
    auto* regs = static_cast<volatile u8*>(vmalloc::map_physical(m.base, m.length));
    return regs && d->bus.init(regs, spi::LpssGen::LynxPoint, 100000000, name);
}

bool eval_int(uacpi_namespace_node* node, const char* name, u64* out) {
    return uacpi_eval_simple_integer(node, name, out) == UACPI_STATUS_OK;
}

bool call_with(uacpi_namespace_node* node, const char* method, u64 value) {
    uacpi_object* arg = uacpi_object_create_integer(value);
    if (!arg)
        return false;
    uacpi_object_array arr{&arg, 1};
    bool ok = uacpi_execute(node, method, &arr) == UACPI_STATUS_OK;
    uacpi_object_unref(arg);
    return ok;
}

void device_thread(void* arg) {
    auto* d = static_cast<AppleSpi*>(arg);
    uacpi_namespace_node* node = d->node;
    // Keyboard and trackpad answer on USB or on SPI; USB already on means
    // the USB driver (bcm5974) has them.
    u64 v = 0;
    if (eval_int(node, "UIST", &v) && v) {
        klog::info("applespi: keyboard/trackpad are on USB on this model; not using SPI");
        return;
    }
    uacpi_namespace_node *sien = nullptr, *sist = nullptr;
    uacpi_namespace_node_find(node, "SIEN", &sien);
    uacpi_namespace_node_find(node, "SIST", &sist);
    if (!sien || !sist) {
        klog::warn("applespi: no SIEN/SIST methods: can't switch the keyboard to SPI");
        return;
    }
    Properties p = read_properties(node);
    if (!p.found)
        klog::info("applespi: no Apple device properties; assuming 8 MHz, mode 0");
    d->mode.speed_hz = p.sclk_period_ns ? static_cast<u32>(1000000000ull / p.sclk_period_ns) : 8000000;
    d->mode.cpol = p.spo != 0;
    d->mode.cpha = p.sph != 0;
    d->cs_delay_us = p.cs_delay_us > 1000 ? 1000 : static_cast<u32>(p.cs_delay_us);
    if (p.word_size != 8 || p.bit_order == 0) {
        klog::warn("applespi: unsupported SPI format (%lu-bit words, %s first)", p.word_size,
                   p.bit_order ? "MSB" : "LSB");
        return;
    }
    u64 gpe = 0;
    if (!eval_int(node, "_GPE", &gpe)) {
        klog::warn("applespi: no _GPE: the keyboard's data-ready signal is unknown");
        return;
    }
    d->gpe = static_cast<u16>(gpe);

    static char ctrl_name[64];
    uacpi_namespace_node* ctrl = uacpi_namespace_node_parent(node);
    if (!ctrl || !setup_controller(d, ctrl, ctrl_name, sizeof(ctrl_name))) {
        klog::warn("applespi: the SPI controller can't be used");
        return;
    }
    // SPI on (USB off); it needs a moment before it takes commands.
    if (!(eval_int(node, "SIST", &v) && v)) {
        if (!call_with(node, "SIEN", 1)) {
            klog::warn("applespi: SIEN(1) failed");
            return;
        }
        sched::sleep_ms(50);
    }
    klog::info("applespi: %s: %u kHz, mode %d, CS delay %u us, data-ready GPE %#x", ctrl_name, d->mode.speed_hz / 1000,
               (d->mode.cpol ? 2 : 0) | (d->mode.cpha ? 1 : 0), d->cs_delay_us, d->gpe);

    if (uacpi_install_gpe_handler(nullptr, d->gpe, UACPI_GPE_TRIGGERING_LEVEL, gpe_handler, d) != UACPI_STATUS_OK) {
        klog::warn("applespi: GPE %#x can't be handled", d->gpe);
        return;
    }
    uacpi_enable_gpe(nullptr, d->gpe);

    // First command: the trackpad's model (and size). A controller that
    // doesn't accept it isn't there or isn't talking: give up cleanly.
    if (!d->request_pad_info()) {
        sched::sleep_ms(100);
        if (!d->request_pad_info()) {
            klog::warn("applespi: the keyboard controller doesn't answer on %s; not used", ctrl_name);
            uacpi_disable_gpe(nullptr, d->gpe);
            uacpi_uninstall_gpe_handler(nullptr, d->gpe, gpe_handler);
            return;
        }
    }
    d->register_keyboard();
    klog::ok("applespi: Apple SPI keyboard on %s", ctrl_name);
    u64 deadline = clock::now_ns() + 1'000'000'000;
    while (!d->pad_up && clock::now_ns() < deadline)
        d->wait_and_handle(100);
    if (!d->pad_up) {
        klog::warn("applespi: no trackpad information; assuming the 12-inch MacBook's trackpad");
        d->register_pad(0x04, 0);
    }
    if (!d->exchange(&AppleSpi::request_multitouch, MSG_MT_INIT))
        klog::warn("applespi: the trackpad didn't switch to multi-touch mode");
    for (;;)
        d->wait_and_handle(60'000);
}

uacpi_namespace_node* g_node = nullptr;

uacpi_iteration_decision found(void*, uacpi_namespace_node* node, uacpi_u32) {
    if (!g_node)
        g_node = node;
    return UACPI_ITERATION_DECISION_BREAK;
}

} // namespace

bool run_selftest() {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) {
            klog::error("applespi self-test: %s", what);
            failures++;
        }
    };
    const u8 digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    check(crc16(digits, sizeof(digits)) == 0xBB3D, "CRC-16/ARC check value");

    // A keyboard report: left shift + 'a' ('A'), Fn up.
    u8 msg[MAX_MSG], pkts[2][PACKET_SIZE];
    const u8 kb[10] = {0, 0x02, 0, 0x04, 0, 0, 0, 0, 0, 0};
    u32 mlen = build_message(msg, MSG_KEYBOARD, 0, 7, 0, kb, sizeof(kb));
    check(build_packets(pkts, PKT_READ, DEV_KEYBOARD, msg, mlen) == 1, "keyboard message in one packet");
    Assembler a;
    const u8* out = nullptr;
    int len = a.feed(pkts[0], &out);
    KeyboardReport r = {};
    check(len == static_cast<int>(mlen) && decode_keyboard(out, static_cast<u32>(len), &r), "keyboard packet accepted");
    check(r.modifiers == 0x02 && r.keys[0] == 0x04 && !r.fn, "keyboard report decoded");
    check(code_to_key(0x04, false) == 30 /* KEY_A */ && code_to_key(0x3A, false) == KEY_BRIGHTNESSDOWN &&
              code_to_key(0x3A, true) == KEY_F1 && code_to_key(0x2A, true) == KEY_DELETE,
          "key translation (Fn layer)");
    pkts[0][20] ^= 1;
    check(a.feed(pkts[0], &out) < 0, "corrupted packet rejected");

    // A touchpad message with 7 fingers (two packets), two touching.
    u8 tp[TP_HEADER + 7 * FINGER_SIZE] = {};
    tp[1] = 1; // clicked
    tp[30] = 7;
    for (int i = 0; i < 7; i++) {
        u8* f = tp + TP_HEADER + i * FINGER_SIZE;
        put16(f + 2, static_cast<u16>(-100 * i));
        put16(f + 4, static_cast<u16>(1000 + i));
        put16(f + 16, i < 2 ? 300 : 0);
    }
    mlen = build_message(msg, MSG_TOUCHPAD, 0, 1, 0, tp, sizeof(tp) - 2);
    // (the last finger's last field is the message CRC, as on the device)
    check(build_packets(pkts, PKT_READ, DEV_TOUCHPAD, msg, mlen) == 2, "touchpad message in two packets");
    Assembler b;
    check(b.feed(pkts[0], &out) == 0, "first packet of two held");
    len = b.feed(pkts[1], &out);
    TouchFrame t = {};
    check(len == static_cast<int>(mlen) && decode_touchpad(out, static_cast<u32>(len), &t), "touchpad message accepted");
    check(t.clicked && t.count == 2 && t.fingers[1].x == -100 && t.fingers[1].y == 1001, "touchpad fingers decoded");
    if (failures)
        return false;
    klog::ok("Apple SPI keyboard/trackpad self-test: CRC, packets, keyboard (Fn layer), two-packet touchpad frame");
    return true;
}

void init() {
    if (!acpi::interpreter_ready())
        return;
    uacpi_find_devices("APP000D", found, nullptr);
    if (!g_node)
        return;
    const char* path = uacpi_namespace_node_generate_absolute_path(g_node);
    klog::info("applespi: Apple SPI keyboard/trackpad %s in ACPI", path ? path : "?");
    if (path)
        uacpi_free_absolute_path(path);
    auto* d = new AppleSpi;
    d->node = g_node;
    sched::spawn("applespi", device_thread, d, -1, true);
}

} // namespace applespi
