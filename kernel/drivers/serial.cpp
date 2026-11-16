#include <arch/x86_64/cpu.hpp>
#include <drivers/serial.hpp>

namespace serial {

namespace {

constexpr u16 COM1 = 0x3F8;

// Register offsets from the base port.
constexpr u16 DATA          = 0; // DLAB=0: rx/tx buffer, DLAB=1: divisor low
constexpr u16 INT_ENABLE    = 1; // DLAB=0: interrupt enable, DLAB=1: divisor high
constexpr u16 FIFO_CTRL     = 2;
constexpr u16 LINE_CTRL     = 3;
constexpr u16 MODEM_CTRL    = 4;
constexpr u16 LINE_STATUS   = 5;

constexpr u8 LSR_TX_EMPTY = 0x20;

bool g_present = false;

} // namespace

bool init() {
    using namespace arch;

    outb(COM1 + INT_ENABLE, 0x00);  // no interrupts
    outb(COM1 + LINE_CTRL, 0x80);   // DLAB on to set baud divisor
    outb(COM1 + DATA, 0x01);        // divisor 1 = 115200 baud
    outb(COM1 + INT_ENABLE, 0x00);
    outb(COM1 + LINE_CTRL, 0x03);   // 8 data bits, no parity, 1 stop bit
    outb(COM1 + FIFO_CTRL, 0xC7);   // enable + clear FIFOs, 14-byte threshold
    outb(COM1 + MODEM_CTRL, 0x1E);  // loopback mode for self-test

    outb(COM1 + DATA, 0xAE);
    if (inb(COM1 + DATA) != 0xAE)
        return g_present = false;   // no UART here (common on modern PCs)

    outb(COM1 + MODEM_CTRL, 0x0F);  // normal mode: DTR, RTS, OUT1, OUT2
    return g_present = true;
}

bool present() { return g_present; }

void putc(char c) {
    if (!g_present)
        return;
    if (c == '\n')
        putc('\r');
    // Bounded wait so a wedged UART can't hang the kernel.
    for (int spins = 0; spins < 100000 && !(arch::inb(COM1 + LINE_STATUS) & LSR_TX_EMPTY); spins++)
        arch::pause();
    arch::outb(COM1 + DATA, static_cast<u8>(c));
}

void write(const char* s, usize len) {
    for (usize i = 0; i < len; i++)
        putc(s[i]);
}

} // namespace serial
