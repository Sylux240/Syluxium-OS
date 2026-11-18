#include <acpi/acpi.hpp>
#include <arch/x86_64/apic.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/interrupts.hpp>
#include <arch/x86_64/percpu.hpp>
#include <core/log.hpp>
#include <core/panic.hpp>
#include <mm/vmalloc.hpp>
#include <time/clock.hpp>

namespace apic {

namespace {

constexpr u32 MSR_APIC_BASE = 0x1B;
constexpr u64 APIC_BASE_ENABLE = 1 << 11;
constexpr u64 APIC_BASE_X2APIC = 1 << 10;

// Register offsets (xAPIC MMIO offset; x2APIC MSR = 0x800 + offset / 16).
constexpr u32 REG_ID          = 0x020;
constexpr u32 REG_TPR         = 0x080;
constexpr u32 REG_EOI         = 0x0B0;
constexpr u32 REG_SVR         = 0x0F0;
constexpr u32 REG_ESR         = 0x280;
constexpr u32 REG_ICR_LOW     = 0x300;
constexpr u32 REG_ICR_HIGH    = 0x310;
constexpr u32 REG_LVT_TIMER   = 0x320;
constexpr u32 REG_LVT_LINT0   = 0x350;
constexpr u32 REG_LVT_LINT1   = 0x360;
constexpr u32 REG_LVT_ERROR   = 0x370;
constexpr u32 REG_TIMER_INIT  = 0x380;
constexpr u32 REG_TIMER_COUNT = 0x390;
constexpr u32 REG_TIMER_DIV   = 0x3E0;

constexpr u32 LVT_MASKED       = 1 << 16;
constexpr u32 LVT_PERIODIC     = 1 << 17;
constexpr u32 LVT_DELIVERY_NMI = 0b100 << 8;
constexpr u32 LVT_ACTIVE_LOW   = 1 << 13;
constexpr u32 LVT_LEVEL        = 1 << 15;
constexpr u32 SVR_ENABLE       = 1 << 8;
constexpr u32 ICR_PENDING      = 1 << 12;
constexpr u32 ICR_ASSERT       = 1 << 14;
constexpr u32 ICR_ALL_BUT_SELF = 0b11 << 18;

bool g_x2apic = false;
volatile u32* g_mmio = nullptr;
u32 g_ticks_per_ms = 0;

u32 read(u32 reg) {
    if (g_x2apic)
        return static_cast<u32>(arch::rdmsr(0x800 + (reg >> 4)));
    return g_mmio[reg / 4];
}

void write(u32 reg, u32 value) {
    if (g_x2apic)
        arch::wrmsr(0x800 + (reg >> 4), value);
    else
        g_mmio[reg / 4] = value;
}

void write_icr(u32 dest, u32 low) {
    if (g_x2apic) {
        arch::wrmsr(0x830, (static_cast<u64>(dest) << 32) | low);
    } else {
        while (read(REG_ICR_LOW) & ICR_PENDING)
            arch::pause();
        write(REG_ICR_HIGH, dest << 24);
        write(REG_ICR_LOW, low);
        while (read(REG_ICR_LOW) & ICR_PENDING)
            arch::pause();
    }
}

void error_handler(interrupts::Frame*) {
    write(REG_ESR, 0);
    klog::warn("APIC error on CPU with LAPIC id %u: ESR=%#x", local_id(), read(REG_ESR));
}

void enable_on_this_cpu() {
    u64 base = arch::rdmsr(MSR_APIC_BASE) | APIC_BASE_ENABLE;
    if (g_x2apic)
        base |= APIC_BASE_X2APIC;
    arch::wrmsr(MSR_APIC_BASE, base);

    write(REG_TPR, 0); // accept all interrupt priorities
    write(REG_LVT_TIMER, LVT_MASKED);
    write(REG_LVT_ERROR, interrupts::VEC_APIC_ERROR);

    // LINT0/LINT1: masked, except where ACPI says an NMI is wired.
    u32 lint[2] = {LVT_MASKED, LVT_MASKED};
    const auto& madt = acpi::madt();
    for (u32 i = 0; i < madt.nmi_count; i++) {
        const auto& n = madt.nmis[i];
        if (n.lint > 1)
            continue;
        // TODO: match processor_uid against this CPU's ACPI UID; all-CPU entries are the common case.
        u32 v = LVT_DELIVERY_NMI;
        if (n.active_low) v |= LVT_ACTIVE_LOW;
        if (n.level_triggered) v |= LVT_LEVEL;
        lint[n.lint] = v;
    }
    write(REG_LVT_LINT0, lint[0]);
    write(REG_LVT_LINT1, lint[1]);

    write(REG_ESR, 0);
    write(REG_ESR, 0);
    write(REG_SVR, SVR_ENABLE | interrupts::VEC_SPURIOUS);
    eoi(); // clear anything left pending
}

} // namespace

void init_bsp() {
    g_x2apic = arch::cpuid(1).ecx & (1u << 21);
    if (!g_x2apic) {
        u64 phys = arch::rdmsr(MSR_APIC_BASE) & 0x000ffffffffff000;
        g_mmio = static_cast<volatile u32*>(vmalloc::map_physical(phys, 4096));
        if (!g_mmio)
            panic("APIC: failed to map registers at %#lx", phys);
    }
    interrupts::set_handler(interrupts::VEC_APIC_ERROR, error_handler);
    enable_on_this_cpu();
    this_cpu()->lapic_id = local_id();
}

void init_ap() { enable_on_this_cpu(); }

u32 local_id() { return g_x2apic ? read(REG_ID) : read(REG_ID) >> 24; }
bool x2apic_mode() { return g_x2apic; }
void eoi() { write(REG_EOI, 0); }

void calibrate_timer() {
    // Count down from the maximum for 10 ms of wall-clock time.
    write(REG_TIMER_DIV, 0x3); // divide by 16
    write(REG_LVT_TIMER, LVT_MASKED);
    write(REG_TIMER_INIT, 0xFFFFFFFF);
    clock::busy_wait_ns(10'000'000);
    u32 elapsed = 0xFFFFFFFF - read(REG_TIMER_COUNT);
    write(REG_TIMER_INIT, 0);
    g_ticks_per_ms = elapsed / 10;
    if (g_ticks_per_ms == 0)
        panic("APIC: timer calibration failed");
}

void start_timer(u32 hz) {
    write(REG_TIMER_DIV, 0x3);
    write(REG_LVT_TIMER, interrupts::VEC_TIMER | LVT_PERIODIC);
    write(REG_TIMER_INIT, g_ticks_per_ms * 1000 / hz);
}

u32 timer_ticks_per_ms() { return g_ticks_per_ms; }

void timer_oneshot_ns(u64 ns) {
    u64 count = ns * g_ticks_per_ms / 1'000'000;
    if (count < 1)
        count = 1;
    if (count > 0xFFFFFFFFull)
        count = 0xFFFFFFFFull;
    write(REG_TIMER_DIV, 0x3);
    write(REG_LVT_TIMER, interrupts::VEC_TIMER); // one-shot
    write(REG_TIMER_INIT, static_cast<u32>(count));
}

void send_ipi(u32 lapic_id, u8 vector) { write_icr(lapic_id, vector | ICR_ASSERT); }

void broadcast_ipi_others(u8 vector) { write_icr(0, vector | ICR_ASSERT | ICR_ALL_BUT_SELF); }

} // namespace apic

// ---------------------------------------------------------------------------

namespace ioapic {

namespace {

constexpr u32 REG_VERSION = 0x01;
constexpr u32 REG_REDIRECTION = 0x10;

constexpr u64 RED_MASKED = 1 << 16;
constexpr u64 RED_LEVEL = 1 << 15;
constexpr u64 RED_ACTIVE_LOW = 1 << 13;

struct IoApic {
    volatile u32* mmio;
    u32 gsi_base;
    u32 inputs;
};

IoApic g_ioapics[acpi::MAX_IOAPICS];
u32 g_count = 0;

u32 read(const IoApic& io, u32 reg) {
    io.mmio[0] = reg;       // IOREGSEL
    return io.mmio[4];      // IOWIN at +0x10
}

void write(const IoApic& io, u32 reg, u32 value) {
    io.mmio[0] = reg;
    io.mmio[4] = value;
}

IoApic* find(u32 gsi) {
    for (u32 i = 0; i < g_count; i++) {
        if (gsi >= g_ioapics[i].gsi_base && gsi < g_ioapics[i].gsi_base + g_ioapics[i].inputs)
            return &g_ioapics[i];
    }
    return nullptr;
}

void write_entry(const IoApic& io, u32 pin, u64 entry) {
    write(io, REG_REDIRECTION + pin * 2 + 1, static_cast<u32>(entry >> 32));
    write(io, REG_REDIRECTION + pin * 2, static_cast<u32>(entry));
}

} // namespace

void init() {
    const auto& madt = acpi::madt();
    for (u32 i = 0; i < madt.ioapic_count; i++) {
        IoApic& io = g_ioapics[g_count];
        io.mmio = static_cast<volatile u32*>(vmalloc::map_physical(madt.ioapics[i].address, 0x20));
        if (!io.mmio)
            continue;
        io.gsi_base = madt.ioapics[i].gsi_base;
        io.inputs = ((read(io, REG_VERSION) >> 16) & 0xFF) + 1;
        for (u32 pin = 0; pin < io.inputs; pin++)
            write_entry(io, pin, RED_MASKED);
        g_count++;
    }
}

bool route_gsi(u32 gsi, u8 vector, u32 dest, bool level, bool active_low) {
    IoApic* io = find(gsi);
    if (!io)
        return false;
    u64 entry = vector | (static_cast<u64>(dest) << 56);
    if (level) entry |= RED_LEVEL;
    if (active_low) entry |= RED_ACTIVE_LOW;
    write_entry(*io, gsi - io->gsi_base, entry);
    return true;
}

bool route_isa_irq(u8 irq, u8 vector, u32 dest) {
    const auto& madt = acpi::madt();
    for (u32 i = 0; i < madt.override_count; i++) {
        const auto& o = madt.overrides[i];
        if (o.isa_irq == irq)
            return route_gsi(o.gsi, vector, dest, o.level_triggered, o.active_low);
    }
    return route_gsi(irq, vector, dest, false, false); // ISA default: edge, active high
}

void mask_gsi(u32 gsi) {
    if (IoApic* io = find(gsi))
        write_entry(*io, gsi - io->gsi_base, RED_MASKED);
}

u32 count() { return g_count; }

} // namespace ioapic
