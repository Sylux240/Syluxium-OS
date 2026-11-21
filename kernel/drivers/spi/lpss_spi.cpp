// Intel LPSS SPI (see lpss_spi.hpp).

#include <core/log.hpp>
#include <drivers/spi/lpss_spi.hpp>
#include <lib/errno.hpp>
#include <time/clock.hpp>

namespace spi {

namespace {

// SSP registers.
constexpr u32 SSCR0 = 0x00, SSCR1 = 0x04, SSSR = 0x08, SSDR = 0x10, SSTO = 0x28, SSPSP = 0x2C;
constexpr u32 SSCR0_DSS_8 = 0x7, SSCR0_SSE = 1u << 7;
constexpr u32 SSCR1_SPO = 1u << 3, SSCR1_SPH = 1u << 4;
constexpr u32 SSSR_TNF = 1u << 2, SSSR_RNE = 1u << 3, SSSR_BSY = 1u << 4, SSSR_ROR = 1u << 7;
// LPSS private registers (from the private block's base).
constexpr u32 PRIV_CLOCK = 0x00, PRIV_RESETS = 0x04;
constexpr u32 CLOCK_ENABLE = 1u << 0, CLOCK_UPDATE = 1u << 31;
constexpr u32 CS_SW_MODE = 1u << 0, CS_HIGH = 1u << 1;

constexpr u64 TIMEOUT_NS = 20'000'000; // a 256-byte packet takes ~0.3 ms at 8 MHz
constexpr u32 IN_FLIGHT = 8;           // bytes written ahead of reads (FIFOs hold 16+)

} // namespace

bool LpssSpi::init(volatile u8* regs, LpssGen gen, u32 clock_hz, const char* name) {
    m_regs = regs;
    m_name = name;
    m_clock_hz = clock_hz;
    if (gen == LpssGen::LynxPoint) {
        m_priv = 0x800;
        m_cs_ctrl = 0x18;
    } else {
        m_priv = 0x200;
        m_cs_ctrl = 0x24;
    }
    // Out of reset (function + APB, and iDMA on Sunrise Point).
    priv_wr(PRIV_RESETS, 0);
    priv_wr(PRIV_RESETS, gen == LpssGen::LynxPoint ? 0x3 : 0x7);
    // The fractional clock divider at M/N = 1/1: the SSP gets the full input
    // clock and divides it itself (SSCR0.SCR). Apple's firmware leaves it to
    // macOS, so it may be off.
    priv_wr(PRIV_CLOCK, CLOCK_UPDATE | (1u << 16) | (1u << 1) | CLOCK_ENABLE);
    // Software chip select, released.
    priv_wr(m_cs_ctrl, (priv_rd(m_cs_ctrl) & ~(CS_SW_MODE | CS_HIGH)) | CS_SW_MODE | CS_HIGH);
    wr(SSCR0, 0);
    // A port that reads back all ones isn't there (or isn't powered).
    u32 sr = rd(SSSR), cr0 = rd(SSCR0);
    if (sr == 0xFFFFFFFF || cr0 == 0xFFFFFFFF) {
        klog::warn("spi: %s: controller doesn't respond (status %#x)", m_name, sr);
        return false;
    }
    klog::info("spi: %s: Intel LPSS SPI (%s), %u MHz input clock", m_name,
               gen == LpssGen::LynxPoint ? "Lynx/Wildcat Point" : "Sunrise Point+", m_clock_hz / 1000000);
    return true;
}

void LpssSpi::configure(const Mode& m) {
    if (m_configured && m.speed_hz == m_mode.speed_hz && m.cpol == m_mode.cpol && m.cpha == m_mode.cpha)
        return;
    u32 speed = m.speed_hz ? m.speed_hz : 1000000;
    u32 div = (m_clock_hz + speed - 1) / speed; // round down the bit rate, never up
    if (div < 1)
        div = 1;
    if (div > 4096)
        div = 4096;
    wr(SSCR0, 0); // off while reconfiguring
    wr(SSTO, 0);
    wr(SSPSP, 0);
    wr(SSCR1, (m.cpol ? SSCR1_SPO : 0) | (m.cpha ? SSCR1_SPH : 0)); // Motorola SPI, no interrupts, no DMA
    wr(SSCR0, ((div - 1) << 8) | SSCR0_DSS_8);
    wr(SSCR0, ((div - 1) << 8) | SSCR0_DSS_8 | SSCR0_SSE);
    m_mode = m;
    m_configured = true;
}

void LpssSpi::cs(bool active) {
    if (!active) {
        // Let the last bits leave before releasing the device.
        u64 deadline = clock::now_ns() + TIMEOUT_NS;
        while ((rd(SSSR) & SSSR_BSY) && clock::now_ns() < deadline) {
        }
    }
    u32 v = priv_rd(m_cs_ctrl) | CS_SW_MODE;
    priv_wr(m_cs_ctrl, active ? (v & ~CS_HIGH) : (v | CS_HIGH));
}

long LpssSpi::transfer(const Mode& m, const u8* tx, u8* rx, u32 len) {
    configure(m);
    // Drop anything left in the receive FIFO.
    for (int i = 0; i < 64 && (rd(SSSR) & SSSR_RNE); i++)
        (void)rd(SSDR);
    wr(SSSR, SSSR_ROR);
    u32 sent = 0, got = 0;
    u64 deadline = clock::now_ns() + TIMEOUT_NS;
    while (got < len) {
        u32 sr = rd(SSSR);
        bool progress = false;
        if (sent < len && sent - got < IN_FLIGHT && (sr & SSSR_TNF)) {
            wr(SSDR, tx ? tx[sent] : 0);
            sent++;
            progress = true;
        }
        if (got < sent && (sr & SSSR_RNE)) {
            u8 b = static_cast<u8>(rd(SSDR));
            if (rx)
                rx[got] = b;
            got++;
            progress = true;
        }
        if (progress)
            deadline = clock::now_ns() + TIMEOUT_NS;
        else if (clock::now_ns() > deadline)
            return -err::TIMEDOUT;
    }
    if (rd(SSSR) & SSSR_ROR) {
        wr(SSSR, SSSR_ROR);
        return -err::IO; // receive overrun: bytes were lost
    }
    return 0;
}

} // namespace spi
