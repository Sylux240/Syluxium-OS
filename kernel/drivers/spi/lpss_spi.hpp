#pragma once

#include <lib/types.hpp>
#include <sched/sync.hpp>

// Intel LPSS SPI host controller: the PXA2xx-style SSP port inside Intel's
// Low Power Subsystem. Laptops use it for Apple's SPI keyboard/touchpad
// (MacBook 2015+, MacBook Pro 2016-2017) and some touchscreens. Two
// generations are handled: Lynx Point/Wildcat Point (Haswell/Broadwell,
// ACPI devices INT33C0/INT33C1/INT3430/INT3431, private registers at
// +0x800) and Sunrise Point and later (Skylake+, PCI functions, private
// registers at +0x200). Polled PIO transfers (no interrupt, no DMA): a
// keyboard/touchpad moves a few 256-byte packets at a time.
// Register facts as documented by Linux's spi-pxa2xx and intel-lpss; our
// own code.
namespace spi {

enum class LpssGen { LynxPoint, SunrisePoint };

struct Mode {
    u32 speed_hz = 8000000;
    bool cpol = false, cpha = false; // clock polarity / phase (SPI mode bits)
};

class LpssSpi {
public:
    // `regs`: the controller's registers (mapped). `clock_hz`: its input
    // clock (120 MHz on Sunrise Point, 100 MHz on Lynx Point). False if the
    // controller doesn't respond like an SSP port.
    bool init(volatile u8* regs, LpssGen gen, u32 clock_hz, const char* name);

    // Clock `len` bytes out of `tx` (zeros if null) and into `rx` (dropped
    // if null). Chip select is the caller's (cs()). 0 or a negative errno
    // (-ETIMEDOUT: the controller stopped moving data).
    long transfer(const Mode& m, const u8* tx, u8* rx, u32 len);
    void cs(bool active); // software chip select (CS0), active low
    const char* name() const { return m_name; }
    sched::Mutex& lock() { return m_lock; } // one device conversation at a time

private:
    u32 rd(u32 reg) const { return *reinterpret_cast<volatile u32*>(m_regs + reg); }
    void wr(u32 reg, u32 v) { *reinterpret_cast<volatile u32*>(m_regs + reg) = v; }
    u32 priv_rd(u32 off) const { return rd(m_priv + off); }
    void priv_wr(u32 off, u32 v) { wr(m_priv + off, v); }
    void configure(const Mode& m);

    volatile u8* m_regs = nullptr;
    u32 m_priv = 0x200, m_cs_ctrl = 0x24;
    u32 m_clock_hz = 120000000;
    Mode m_mode;
    bool m_configured = false;
    const char* m_name = "spi";
    sched::Mutex m_lock;
};

} // namespace spi
