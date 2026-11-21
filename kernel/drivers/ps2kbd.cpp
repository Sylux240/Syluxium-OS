#include <core/log.hpp>
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
#include <boot/limine_requests.hpp>
#include <drivers/input/input.hpp>
#include <drivers/input/ps2_touchpad.hpp>
#include <lib/string.hpp>
#include <drivers/ps2kbd.hpp>
#include <drivers/tty.hpp>
#include <lib/spinlock.hpp>
#include <sched/sched.hpp>
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
    case 0x37: return 99;  // KEY_SYSRQ (PrintScreen)
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

void mouse_packet(u8 flags, u8 b1, u8 b2, u8 b3);

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
    u16 keycode = g_extended ? extended_keycode(code) : code == 0x54 ? 99 /* Alt+PrintScreen: KEY_SYSRQ */ : code;
    g_extended = false;
    if (!keycode)
        return;
    g_keyboard.report(input::EV_KEY, keycode, release ? 0 : 1);
    g_keyboard.sync();
    console_key(keycode, !release);
}

void mouse_byte(u8 b) {
    if (ps2pad::active()) {
        ps2pad::byte(b);
        return;
    }
    if (g_packet_len == 0 && !(b & 0x08))
        return; // out of sync: the first byte always has bit 3 set
    g_packet[g_packet_len++] = b;
    if (g_packet_len < g_packet_size)
        return;
    g_packet_len = 0;
    mouse_packet(g_packet[0], g_packet[1], g_packet[2], g_packet_size == 4 ? g_packet[3] : 0);
}

// Pointer motion and buttons (bit 0 left, 1 right, 2 middle); dy grows downwards.
void stick_report(u8 buttons, i32 dx, i32 dy) {
    if (dx) g_mouse.report(input::EV_REL, input::REL_X, dx);
    if (dy) g_mouse.report(input::EV_REL, input::REL_Y, dy);
    static const u16 codes[3] = {input::BTN_LEFT, input::BTN_RIGHT, input::BTN_MIDDLE};
    for (int i = 0; i < 3; i++)
        if ((buttons ^ g_buttons) & (1 << i))
            g_mouse.report(input::EV_KEY, codes[i], (buttons >> i) & 1);
    g_buttons = buttons & 7;
    g_mouse.sync();
}

void mouse_packet(u8 flags, u8 b1, u8 b2, u8 b3) {
    if (flags & 0xC0)
        return; // overflow: drop the packet
    i32 dx = b1 - ((flags & 0x10) ? 256 : 0);
    i32 dy = b2 - ((flags & 0x20) ? 256 : 0);
    i32 wheel = static_cast<i8>(static_cast<i8>(b3 << 4) >> 4);
    if (wheel) g_mouse.report(input::EV_REL, input::REL_WHEEL, -wheel);
    stick_report(flags & 7, dx, -dy); // PS/2: up is positive
}

// The interrupt handler and the polling fallback both drain the
// controller; whoever holds the lock does it.
RawSpinlock g_drain_lock;
bool g_initialized = false;
u64 g_irq_count = 0, g_polled_bytes = 0;

void drain_locked(bool polled) {
    while (arch::inb(STATUS) & STATUS_OUTPUT_FULL) {
        u8 status = arch::inb(STATUS);
        u8 b = arch::inb(DATA);
        if (polled)
            g_polled_bytes++;
        if (status & STATUS_AUX) {
            if (g_have_mouse)
                mouse_byte(b);
        } else {
            keyboard_byte(b);
        }
    }
}

void irq(interrupts::Frame*) {
    g_irq_count++;
    if (!g_drain_lock.try_lock())
        return; // the poller or the late setup has it
    drain_locked(false);
    g_drain_lock.unlock();
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

void flush_output() {
    for (int i = 0; i < 256 && (arch::inb(STATUS) & STATUS_OUTPUT_FULL); i++) {
        arch::inb(DATA);
        arch::io_wait();
    }
}

bool init_mouse() {
    controller_cmd(0xA8); // enable the aux port
    flush_output();
    mouse_cmd(0xFF);      // reset (touchpads come up in plain PS/2 mouse mode)
    {
        u8 b;
        u64 deadline = clock::now_ns() + 1'000'000'000;
        while (clock::now_ns() < deadline && !(read_byte(&b) && b == 0xAA)) {}
        read_byte(&b);    // device id after the self-test
    }
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
    static const ps2pad::Port port = {mouse_cmd, read_byte, stick_report};
    if (ps2pad::probe(port))
        return mouse_cmd(0xF4); // start streaming
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

// Send a byte to the keyboard and wait for its ACK (0xFA; 0xFE = resend).
bool keyboard_cmd(u8 b) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!wait_write())
            return false;
        arch::outb(DATA, b);
        u8 r;
        if (!read_byte(&r))
            return false;
        if (r == 0xFA)
            return true;
        if (r != 0xFE)
            return false;
    }
    return false;
}

// Set the controller up the way Linux's i8042 driver does. Firmware leaves
// it in all sorts of states: UEFI firmware often polls the keyboard and
// leaves its interrupt disabled, or the keyboard port disabled entirely.
void setup_controller() {
    controller_cmd(0xAD); // disable the keyboard port
    controller_cmd(0xA7); // disable the aux port
    flush_output();
    controller_cmd(0x20);
    u8 config = 0x45;     // fallback if the read fails
    bool got_config = read_byte(&config);
    u8 firmware_config = config;
    config &= ~0x03;      // interrupts off while we set up
    config |= 0x40;       // scancode translation (set 2 -> set 1)
    config &= ~0x10;      // keyboard clock on
    controller_cmd(0x60);
    wait_write();
    arch::outb(DATA, config);
    // Self-test resets some controllers' config; many laptops answer
    // nonsense or nothing at all. Neither is fatal (Linux ignores it too).
    controller_cmd(0xAA);
    u8 result = 0;
    read_byte(&result);
    controller_cmd(0x60);
    wait_write();
    arch::outb(DATA, config);
    controller_cmd(0xAE); // enable the keyboard port
    flush_output();
    klog::info("PS/2 controller: firmware config %#x%s (keyboard IRQ %s), self-test %#x", firmware_config,
               got_config ? "" : " (unreadable)", (firmware_config & 1) ? "on" : "OFF", result);
    // The keyboard itself: reset (0xFF -> ACK, 0xAA), then scanning on.
    if (keyboard_cmd(0xFF)) {
        u8 bat = 0;
        u64 deadline = clock::now_ns() + 1'000'000'000;
        while (clock::now_ns() < deadline && !(read_byte(&bat) && (bat == 0xAA || bat == 0xFC))) {}
    }
    keyboard_cmd(0xF4); // enable scanning
    flush_output();
    // Keyboard interrupt on (the aux interrupt is set by init_mouse()).
    controller_cmd(0x20);
    if (read_byte(&config)) {
        config |= 0x01;
        config |= 0x40;
        config &= ~0x10;
        controller_cmd(0x60);
        wait_write();
        arch::outb(DATA, config);
    }
}

bool init() {
    // No controller (common on new UEFI-only PCs) reads back as 0xFF.
    if (arch::inb(STATUS) == 0xFF)
        return false;
    flush_output();
    setup_controller();

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
    g_initialized = true;
    return true;
}

namespace {

// Safety net for machines where the keyboard's interrupt never arrives
// (routing, firmware): bytes left in the controller are picked up here.
void poll_thread(void*) {
    u64 start = clock::now_ns();
    bool reported30 = false, reported120 = false;
    for (;;) {
        bool irqs_work = g_irq_count > 0 && g_polled_bytes == 0;
        sched::sleep_ms(irqs_work ? 250 : 20);
        if (arch::inb(STATUS) & STATUS_OUTPUT_FULL) {
            bool was = arch::interrupts_enabled();
            arch::cli();
            if (g_drain_lock.try_lock()) {
                drain_locked(true);
                g_drain_lock.unlock();
            }
            if (was)
                arch::sti();
        }
        u64 secs = (clock::now_ns() - start) / 1'000'000'000;
        if ((secs >= 30 && !reported30) || (secs >= 120 && !reported120)) {
            (secs >= 120 ? reported120 : reported30) = true;
            klog::info("PS/2: %lu interrupt(s), %lu byte(s) picked up by polling%s", g_irq_count, g_polled_bytes,
                       g_polled_bytes && !g_irq_count ? " - the keyboard interrupt never arrives" : "");
        }
    }
}

} // namespace

// After the USB controllers are taken from the firmware: its PS/2
// emulation (which intercepts ports 0x60/0x64) is gone and may have left
// the real controller with the keyboard or its interrupt off - set it up
// again, like Linux, which takes USB over before touching PS/2.
void late_init() {
    if (!g_initialized) {
        if (!init())
            return;
        klog::ok("PS/2 controller found after the USB handoff");
    } else {
        bool was = arch::interrupts_enabled();
        arch::cli();
        g_drain_lock.lock();
        flush_output();
        setup_controller();
        if (g_have_mouse)
            init_mouse();
        for (int i = 0; i < 64 && (arch::inb(STATUS) & STATUS_OUTPUT_FULL); i++)
            arch::inb(DATA);
        g_drain_lock.unlock();
        if (was)
            arch::sti();
    }
    sched::spawn("ps2-poll", poll_thread, nullptr, -1, true);
}

bool mouse_present() { return g_have_mouse; }

} // namespace ps2kbd
