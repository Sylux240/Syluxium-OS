#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/pic.hpp>

namespace pic {

namespace {
constexpr u16 PIC1_CMD = 0x20, PIC1_DATA = 0x21;
constexpr u16 PIC2_CMD = 0xA0, PIC2_DATA = 0xA1;

constexpr u8 ICW1_INIT = 0x10, ICW1_ICW4 = 0x01;
constexpr u8 ICW4_8086 = 0x01;
} // namespace

void disable() {
    using namespace arch;

    // Remap: master IRQ 0-7 -> vectors 0x20-0x27, slave IRQ 8-15 -> 0x28-0x2F,
    // so any spurious PIC interrupt can't be mistaken for a CPU exception.
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();
    outb(PIC2_DATA, 0x28); io_wait();
    outb(PIC1_DATA, 4); io_wait(); // slave on IRQ2
    outb(PIC2_DATA, 2); io_wait(); // slave cascade identity
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    // Mask everything.
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

} // namespace pic
