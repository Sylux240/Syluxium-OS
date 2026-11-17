// i8042 PS/2 controller: keyboard (IRQ 1) and mouse (IRQ 12).
//
// Both devices share the data port, so one handler drains it and routes
// each byte by the "came from the aux (mouse) port" status bit. The
// keyboard feeds the console tty (characters) and an evdev device (Linux
// key codes); the mouse feeds an evdev device (motion, buttons, wheel).

#include <arch/x86_64/apic.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/percpu.hpp>
#include <drivers/input/input.hpp>
#include <drivers/ps2kbd.hpp>
#include <drivers/tty.hpp>
#include <time/clock.hpp>

namespace ps2kbd {

void console_key(u16 keycode, bool pressed);

namespace {

constexpr u16 DATA = 0x60, STATUS = 0x64, COMMAND = 0x64;
constexpr u8 STATUS_OUTPUT_FULL = 1, STATUS_INPUT_FULL = 2, STATUS_AUX = 0x20;

// Scancode set 1 (the controller translates the keyboard's set 2 for us).
constexpr char normal_map[128] = {
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q',  'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,   'a', 's',
    'd',  'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,  '\\', 'z', 'x', 'c', 'v',
    'b',  'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,
};
constexpr char shift_map[128] = {
    0,    27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,   'A', 'S',
    'D',  'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
    'B',  'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,
};


// Extended (E0-prefixed) scancodes -> Linux key codes.
u16 extended_keycode(u8 code) {
    switch (code) {
    case 0x1C: return 96;  // KEY_KPENTER
    case 0x1D: return 97;  // KEY_RIGHTCTRL
    case 0x35: return 98;  // KEY_KPSLASH
    case 0x38: return 100; // KEY_RIGHTALT
    case 0x47: return 102; // KEY_HOME
    case 0x48: return 103; // KEY_UP
    case 0x49: return 104; // KEY_PAGEUP
    case 0x4B: return 105; // KEY_LEFT
    case 0x4D: return 106; // KEY_RIGHT
    case 0x4F: return 107; // KEY_END
    case 0x50: return 108; // KEY_DOWN
    case 0x51: return 109; // KEY_PAGEDOWN
    case 0x52: return 110; // KEY_INSERT
    case 0x53: return 111; // KEY_DELETE
    case 0x5B: return 125; // KEY_LEFTMETA (Windows key)
    case 0x5C: return 126; // KEY_RIGHTMETA
    case 0x5D: return 127; // KEY_COMPOSE (menu key)
    default:   return 0;
    }
}

input::Device g_keyboard;
bool g_console_shift = false, g_console_ctrl = false, g_console_alt = false, g_console_caps = false;
input::Device g_mouse;
bool g_have_mouse = false;
bool g_extended = false;

// Mouse packet assembly.
u8 g_packet[4];
int g_packet_len = 0, g_packet_size = 3;
u8 g_buttons = 0;

void emit(const char* s) {
    while (*s)
        tty::input_char(*s++);
}

void keyboard_byte(u8 sc) {
    if (sc == 0xE0) {
        g_extended = true;
        return;
    }
    bool release = sc & 0x80;
    u8 code = sc & 0x7F;
    // Set-1 scancodes 1..88 are the Linux key codes themselves; E0 ones need a table.
    u16 keycode = g_extended ? extended_keycode(code) : code;
    g_extended = false;
    if (!keycode)
        return;
    g_keyboard.report(input::EV_KEY, keycode, release ? 0 : 1);
    g_keyboard.sync();
    console_key(keycode, !release);
}

void mouse_byte(u8 b) {
    if (g_packet_len == 0 && !(b & 0x08))
        return; // out of sync: the first byte always has bit 3 set
    g_packet[g_packet_len++] = b;
    if (g_packet_len < g_packet_size)
        return;
    g_packet_len = 0;

    u8 flags = g_packet[0];
    if (flags & 0xC0)
        return; // overflow: drop the packet
    i32 dx = g_packet[1] - ((flags & 0x10) ? 256 : 0);
    i32 dy = g_packet[2] - ((flags & 0x20) ? 256 : 0);
    i32 wheel = g_packet_size == 4 ? static_cast<i8>(static_cast<i8>(g_packet[3] << 4) >> 4) : 0;

    if (dx) g_mouse.report(input::EV_REL, input::REL_X, dx);
    if (dy) g_mouse.report(input::EV_REL, input::REL_Y, -dy); // PS/2: up is positive
    if (wheel) g_mouse.report(input::EV_REL, input::REL_WHEEL, -wheel);
    u8 buttons = flags & 7;
    static const u16 codes[3] = {input::BTN_LEFT, input::BTN_RIGHT, input::BTN_MIDDLE};
    for (int i = 0; i < 3; i++)
        if ((buttons ^ g_buttons) & (1 << i))
            g_mouse.report(input::EV_KEY, codes[i], (buttons >> i) & 1);
    g_buttons = buttons;
    g_mouse.sync();
}

void irq(interrupts::Frame*) {
    while (arch::inb(STATUS) & STATUS_OUTPUT_FULL) {
        u8 status = arch::inb(STATUS);
        u8 b = arch::inb(DATA);
        if (status & STATUS_AUX) {
            if (g_have_mouse)
                mouse_byte(b);
        } else {
            keyboard_byte(b);
        }
    }
}

// ---- controller commands (polled, before interrupts are routed) ----

bool wait_write() {
    u64 deadline = clock::now_ns() + 50'000'000;
    while (arch::inb(STATUS) & STATUS_INPUT_FULL)
        if (clock::now_ns() > deadline)
            return false;
    return true;
}

bool read_byte(u8* out) {
    u64 deadline = clock::now_ns() + 50'000'000;
    while (!(arch::inb(STATUS) & STATUS_OUTPUT_FULL))
        if (clock::now_ns() > deadline)
            return false;
    *out = arch::inb(DATA);
    return true;
}

void controller_cmd(u8 cmd) {
    wait_write();
    arch::outb(COMMAND, cmd);
}

// Send a byte to the mouse and wait for its ACK.
bool mouse_cmd(u8 b) {
    controller_cmd(0xD4); // next data byte goes to the aux port
    wait_write();
    arch::outb(DATA, b);
    u8 ack;
    return read_byte(&ack) && ack == 0xFA;
}

bool init_mouse() {
    controller_cmd(0xA8); // enable the aux port
    controller_cmd(0x20); // read config byte
    u8 config;
    if (!read_byte(&config))
        return false;
    config |= 0x02;        // aux interrupt on
    config &= ~0x20;       // aux clock on
    controller_cmd(0x60);
    wait_write();
    arch::outb(DATA, config);

    if (!mouse_cmd(0xF6)) // defaults
        return false;
    // IntelliMouse handshake: sample rates 200, 100, 80 unlock the wheel (ID 3).
    static const u8 rates[] = {200, 100, 80};
    for (u8 rate : rates) {
        mouse_cmd(0xF3);
        mouse_cmd(rate);
    }
    mouse_cmd(0xF2);
    u8 id = 0;
    read_byte(&id);
    g_packet_size = id == 3 || id == 4 ? 4 : 3;
    return mouse_cmd(0xF4); // start streaming
}

} // namespace

// Translate a Linux key code into characters for the console terminal.
void console_key(u16 keycode, bool pressed) {
    if (!tty::keyboard_to_tty())
        return; // an X server reads the keyboard through /dev/input instead
    switch (keycode) {
    case 42: case 54: g_console_shift = pressed; return;   // shifts
    case 29: case 97: g_console_ctrl = pressed; return;    // ctrls
    case 56: case 100: g_console_alt = pressed; return;    // alts
    case 58: if (pressed) g_console_caps = !g_console_caps; return;
    default: break;
    }
    if (!pressed)
        return;
    switch (keycode) {
    case 103: emit("\x1b[A"); return;  // up
    case 108: emit("\x1b[B"); return;  // down
    case 106: emit("\x1b[C"); return;  // right
    case 105: emit("\x1b[D"); return;  // left
    case 102: emit("\x1b[H"); return;  // home
    case 107: emit("\x1b[F"); return;  // end
    case 104: emit("\x1b[5~"); return; // page up
    case 109: emit("\x1b[6~"); return; // page down
    case 110: emit("\x1b[2~"); return; // insert
    case 111: emit("\x1b[3~"); return; // delete
    case 96: tty::input_char('\n'); return; // keypad enter
    case 98: tty::input_char('/'); return;   // keypad /
    default: break;
    }
    if (keycode >= sizeof(normal_map))
        return;
    char c = g_console_shift ? shift_map[keycode] : normal_map[keycode];
    if (!c)
        return;
    if (g_console_caps && c >= 'a' && c <= 'z' && !g_console_shift) c -= 32;
    else if (g_console_caps && c >= 'A' && c <= 'Z' && g_console_shift) c += 32;
    if (g_console_ctrl) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 1);
        else if (c == '\\') c = 0x1C;
        else return;
    }
    if (g_console_alt)
        tty::input_char(0x1B); // Alt+key = ESC prefix, like Linux consoles
    tty::input_char(c);
}

bool init() {
    // No controller (common on new UEFI-only PCs) reads back as 0xFF.
    if (arch::inb(STATUS) == 0xFF)
        return false;
    for (int i = 0; i < 64 && (arch::inb(STATUS) & STATUS_OUTPUT_FULL); i++)
        arch::inb(DATA); // drain stale bytes

    const char kname[] = "AT Translated Set 2 keyboard"; // what Linux calls it
    for (usize i = 0; i < sizeof(kname); i++)
        g_keyboard.name[i] = kname[i];
    g_keyboard.bustype = input::BUS_I8042;
    g_keyboard.vendor = 1;
    g_keyboard.product = 1;
    g_keyboard.ev_bits = (1ull << input::EV_KEY) | (1ull << input::EV_REP);
    for (u16 k = 1; k < 128; k++)
        g_keyboard.set_key(k);
    input::register_device(&g_keyboard);

    g_have_mouse = init_mouse();
    if (g_have_mouse) {
        const char mname[] = "PS/2 Generic Mouse";
        for (usize i = 0; i < sizeof(mname); i++) g_mouse.name[i] = mname[i];
        g_mouse.bustype = input::BUS_I8042;
        g_mouse.vendor = 2;
        g_mouse.product = g_packet_size == 4 ? 3 : 1;
        g_mouse.ev_bits = (1ull << input::EV_KEY) | (1ull << input::EV_REL);
        g_mouse.rel_bits = (1ull << input::REL_X) | (1ull << input::REL_Y) | (1ull << input::REL_WHEEL);
        g_mouse.set_key(input::BTN_LEFT);
        g_mouse.set_key(input::BTN_RIGHT);
        g_mouse.set_key(input::BTN_MIDDLE);
        input::register_device(&g_mouse);
    }
    for (int i = 0; i < 64 && (arch::inb(STATUS) & STATUS_OUTPUT_FULL); i++)
        arch::inb(DATA);

    u8 vector = interrupts::allocate_vector();
    interrupts::set_handler(vector, irq);
    ioapic::route_isa_irq(1, vector, this_cpu()->lapic_id);
    if (g_have_mouse)
        ioapic::route_isa_irq(12, vector, this_cpu()->lapic_id); // same handler: it checks the aux bit
    return true;
}

bool mouse_present() { return g_have_mouse; }

} // namespace ps2kbd
