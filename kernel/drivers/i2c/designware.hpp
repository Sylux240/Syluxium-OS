#pragma once

#include <lib/types.hpp>
#include <sched/sync.hpp>

// Synopsys DesignWare I2C host controller: what laptops use for their I2C
// touchpads, touchscreens and sensors - Intel's LPSS (PCI functions, since
// Haswell/Skylake) and AMD's FCH (ACPI devices AMDI0010/AMDI0019).
// Polled transfers (no interrupt, no DMA): a few dozen bytes per touchpad
// report. Register facts as documented by Linux's i2c-designware driver;
// our own code.
namespace i2c {

struct Timing {
    u16 hcnt = 0, lcnt = 0, sda_hold = 0; // 0: computed / left as the firmware set it
};

class DesignWare {
public:
    // `regs`: the controller's registers (mapped). `clock_khz`: its input
    // clock (for timings ACPI doesn't give). `speed_hz`: 100000 or 400000+.
    bool init(volatile u8* regs, u32 clock_khz, u32 speed_hz, const Timing& t, const char* name);
    // Write `wlen` bytes to the device at 7-bit address `addr`, then (with a
    // repeated start) read `rlen` bytes. Either part may be empty.
    // 0 on success, negative errno (-ENXIO: no ACK from the device).
    long transfer(u16 addr, const u8* w, u32 wlen, u8* r, u32 rlen);
    const char* name() const { return m_name; }

private:
    u32 rd(u32 reg) const { return *reinterpret_cast<volatile u32*>(m_regs + reg); }
    void wr(u32 reg, u32 v) { *reinterpret_cast<volatile u32*>(m_regs + reg) = v; }
    bool set_enabled(bool on);

    volatile u8* m_regs = nullptr;
    u32 m_tx_depth = 8, m_rx_depth = 8;
    u32 m_con = 0;
    const char* m_name = "i2c";
    sched::Mutex m_lock; // one transfer at a time (several devices may share a bus)
};

} // namespace i2c
