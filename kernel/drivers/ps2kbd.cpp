#include <arch/x86_64/apic.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/percpu.hpp>
#include <drivers/ps2kbd.hpp>
#include <drivers/tty.hpp>

namespace ps2kbd {

namespace {

constexpr u16 DATA = 0x60, STATUS = 0x64;
constexpr u8 STATUS_OUTPUT_FULL = 1, STATUS_AUX = 0x20;

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

constexpr u8 SC_LCTRL = 0x1D, SC_LSHIFT = 0x2A, SC_RSHIFT = 0x36, SC_LALT = 0x38, SC_CAPS = 0x3A;

bool g_shift = false, g_ctrl = false, g_alt = false, g_caps = false, g_extended = false;

void emit(const char* s) {
    while (*s)
        tty::input_char(*s++);
}

void handle(u8 sc) {
    if (sc == 0xE0) {
        g_extended = true;
        return;
    }
    bool release = sc & 0x80;
    u8 code = sc & 0x7F;

    if (g_extended) {
        g_extended = false;
        if (code == SC_LCTRL) { g_ctrl = !release; return; }      // right Ctrl
        if (code == SC_LALT) { g_alt = !release; return; }        // right Alt
        if (release)
            return;
        switch (code) { // arrows and friends: the escape sequences a VT100/Linux console sends
        case 0x48: emit("\x1b[A"); break;  // up
        case 0x50: emit("\x1b[B"); break;  // down
        case 0x4D: emit("\x1b[C"); break;  // right
        case 0x4B: emit("\x1b[D"); break;  // left
        case 0x47: emit("\x1b[H"); break;  // home
        case 0x4F: emit("\x1b[F"); break;  // end
        case 0x53: emit("\x1b[3~"); break; // delete
        case 0x1C: tty::input_char('\n'); break; // keypad enter
        case 0x35: tty::input_char('/'); break;  // keypad /
        default: break;
        }
        return;
    }

    switch (code) {
    case SC_LSHIFT: case SC_RSHIFT: g_shift = !release; return;
    case SC_LCTRL: g_ctrl = !release; return;
    case SC_LALT: g_alt = !release; return;
    case SC_CAPS: if (!release) g_caps = !g_caps; return;
    default: break;
    }
    if (release || code >= sizeof(normal_map))
        return;

    char c = g_shift ? shift_map[code] : normal_map[code];
    if (!c)
        return;
    if (g_caps && c >= 'a' && c <= 'z' && !g_shift) c -= 32;
    else if (g_caps && c >= 'A' && c <= 'Z' && g_shift) c += 32;
    if (g_ctrl) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 1);
        else if (c == '\\') c = 0x1C;
        else return;
    }
    if (g_alt)
        tty::input_char(0x1B); // Alt+key = ESC prefix, like Linux consoles
    tty::input_char(c);
}

void irq(interrupts::Frame*) {
    while (arch::inb(STATUS) & STATUS_OUTPUT_FULL) {
        u8 status = arch::inb(STATUS);
        u8 sc = arch::inb(DATA);
        if (!(status & STATUS_AUX)) // ignore mouse bytes
            handle(sc);
    }
}

} // namespace

bool init() {
    // No controller (common on new UEFI-only PCs) reads back as 0xFF.
    if (arch::inb(STATUS) == 0xFF)
        return false;
    for (int i = 0; i < 64 && (arch::inb(STATUS) & STATUS_OUTPUT_FULL); i++)
        arch::inb(DATA); // drain stale bytes
    u8 vector = interrupts::allocate_vector();
    interrupts::set_handler(vector, irq);
    ioapic::route_isa_irq(1, vector, this_cpu()->lapic_id);
    return true;
}

} // namespace ps2kbd
