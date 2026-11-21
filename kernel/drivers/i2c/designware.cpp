// DesignWare I2C host controller, polled (see designware.hpp).

#include <arch/x86_64/cpu.hpp>
#include <core/log.hpp>
#include <drivers/i2c/designware.hpp>
#include <lib/errno.hpp>
#include <time/clock.hpp>

namespace i2c {

namespace {

constexpr u32 IC_CON = 0x00, IC_TAR = 0x04, IC_DATA_CMD = 0x10, IC_SS_SCL_HCNT = 0x14, IC_SS_SCL_LCNT = 0x18,
              IC_FS_SCL_HCNT = 0x1C, IC_FS_SCL_LCNT = 0x20, IC_INTR_MASK = 0x30, IC_RAW_INTR_STAT = 0x34,
              IC_RX_TL = 0x38, IC_TX_TL = 0x3C, IC_CLR_INTR = 0x40, IC_CLR_TX_ABRT = 0x54, IC_CLR_STOP_DET = 0x60,
              IC_ENABLE = 0x6C, IC_STATUS = 0x70, IC_TXFLR = 0x74, IC_RXFLR = 0x78, IC_SDA_HOLD = 0x7C,
              IC_TX_ABRT_SOURCE = 0x80, IC_ENABLE_STATUS = 0x9C, IC_COMP_PARAM_1 = 0xF4, IC_COMP_TYPE = 0xFC;
constexpr u32 CON_MASTER = 1 << 0, CON_SPEED_STD = 1 << 1, CON_SPEED_FAST = 2 << 1, CON_RESTART_EN = 1 << 5,
              CON_SLAVE_DISABLE = 1 << 6;
constexpr u32 CMD_READ = 1 << 8, CMD_STOP = 1 << 9, CMD_RESTART = 1 << 10;
constexpr u32 INTR_TX_ABRT = 1 << 6, INTR_STOP_DET = 1 << 9;
constexpr u32 STATUS_ACTIVITY = 1 << 0;
constexpr u32 COMP_TYPE_DW = 0x44570140;

// Linux's i2c_dw_scl_hcnt/lcnt (clock in kHz, times in ns).
u32 scl_hcnt(u32 khz, u32 t_high, u32 t_fall) { return static_cast<u32>((u64(khz) * (t_high + t_fall) + 500000) / 1000000) - 3; }
u32 scl_lcnt(u32 khz, u32 t_low, u32 t_fall) { return static_cast<u32>((u64(khz) * (t_low + t_fall) + 500000) / 1000000) - 1; }

} // namespace

bool DesignWare::set_enabled(bool on) {
    // Disabling waits for the current transfer to end (up to ~10 ms).
    for (int i = 0; i < 1000; i++) {
        wr(IC_ENABLE, on ? 1 : 0);
        if ((rd(IC_ENABLE_STATUS) & 1) == (on ? 1u : 0u))
            return true;
        u64 until = clock::now_ns() + 10'000;
        while (clock::now_ns() < until)
            arch::pause();
    }
    return false;
}

bool DesignWare::init(volatile u8* regs, u32 clock_khz, u32 speed_hz, const Timing& t, const char* name) {
    m_regs = regs;
    m_name = name;
    u32 type = rd(IC_COMP_TYPE);
    if (type != COMP_TYPE_DW) {
        klog::warn("i2c: %s: not a DesignWare controller (component type %#x)", name, type);
        return false;
    }
    u32 param = rd(IC_COMP_PARAM_1);
    m_tx_depth = ((param >> 16) & 0xFF) + 1;
    m_rx_depth = ((param >> 8) & 0xFF) + 1;
    if (param == 0 || m_tx_depth < 2 || m_rx_depth < 2) // some cores don't fill the parameter register
        m_tx_depth = m_rx_depth = 32;
    if (!set_enabled(false)) {
        klog::warn("i2c: %s: controller does not stop", name);
        return false;
    }
    bool fast = speed_hz > 100000;
    u32 hcnt = t.hcnt, lcnt = t.lcnt;
    if (!hcnt || !lcnt) {
        hcnt = fast ? scl_hcnt(clock_khz, 600, 300) : scl_hcnt(clock_khz, 4000, 300);
        lcnt = fast ? scl_lcnt(clock_khz, 1300, 300) : scl_lcnt(clock_khz, 4700, 300);
    }
    wr(fast ? IC_FS_SCL_HCNT : IC_SS_SCL_HCNT, hcnt);
    wr(fast ? IC_FS_SCL_LCNT : IC_SS_SCL_LCNT, lcnt);
    if (t.sda_hold)
        wr(IC_SDA_HOLD, t.sda_hold);
    m_con = CON_MASTER | CON_SLAVE_DISABLE | CON_RESTART_EN | (fast ? CON_SPEED_FAST : CON_SPEED_STD);
    wr(IC_CON, m_con);
    wr(IC_TX_TL, 0);
    wr(IC_RX_TL, 0);
    wr(IC_INTR_MASK, 0); // polled
    rd(IC_CLR_INTR);
    klog::info("i2c: %s: DesignWare controller, %s mode (hcnt %u, lcnt %u, FIFOs %u/%u)", name,
               fast ? "fast" : "standard", hcnt, lcnt, m_tx_depth, m_rx_depth);
    return true;
}

long DesignWare::transfer(u16 addr, const u8* w, u32 wlen, u8* r, u32 rlen) {
    if (!m_regs || (!wlen && !rlen))
        return -err::INVAL;
    m_lock.lock();
    long result = 0;
    if (!set_enabled(false)) {
        m_lock.unlock();
        return -err::IO;
    }
    wr(IC_TAR, addr & 0x7F);
    wr(IC_INTR_MASK, 0);
    set_enabled(true);
    rd(IC_CLR_INTR);

    u32 total = wlen + rlen, sent = 0, received = 0;
    // ~25 us per byte at 400 kHz; generous for slow (100 kHz) buses and stretching devices.
    u64 deadline = clock::now_ns() + 20'000'000 + static_cast<u64>(total) * 200'000;
    for (;;) {
        u32 raw = rd(IC_RAW_INTR_STAT);
        if (raw & INTR_TX_ABRT) {
            u32 src = rd(IC_TX_ABRT_SOURCE);
            rd(IC_CLR_TX_ABRT);
            result = (src & 0xF) ? -err::NXIO : -err::IO; // bits 0-3: address/data not acknowledged
            break;
        }
        // Keep the TX FIFO fed: when it runs empty the controller ends the transfer.
        while (sent < total && rd(IC_TXFLR) < m_tx_depth && (sent < wlen || sent - wlen - received < m_rx_depth)) {
            u32 cmd;
            if (sent < wlen) {
                cmd = w[sent];
                if (sent == wlen - 1 && !rlen)
                    cmd |= CMD_STOP;
            } else {
                u32 i = sent - wlen;
                cmd = CMD_READ;
                if (i == 0 && wlen)
                    cmd |= CMD_RESTART;
                if (i == rlen - 1)
                    cmd |= CMD_STOP;
            }
            wr(IC_DATA_CMD, cmd);
            sent++;
        }
        while (received < rlen && rd(IC_RXFLR))
            r[received++] = static_cast<u8>(rd(IC_DATA_CMD));
        if (sent == total && received == rlen && (raw & INTR_STOP_DET) && !(rd(IC_STATUS) & STATUS_ACTIVITY))
            break;
        if (clock::now_ns() > deadline) {
            result = -err::TIMEDOUT;
            break;
        }
        arch::pause();
    }
    rd(IC_CLR_STOP_DET);
    set_enabled(false);
    m_lock.unlock();
    return result;
}

} // namespace i2c
