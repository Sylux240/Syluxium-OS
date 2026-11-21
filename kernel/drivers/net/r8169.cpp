// Realtek RTL8169 / RTL8168 / RTL8111 / RTL8101 (r8169 family) Gigabit and
// Fast Ethernet: the network chip on most desktop mainboards.
//
// *** UNTESTED ON HARDWARE. *** QEMU cannot emulate these chips, so this
// driver was written from the documented register interface (RTL8169
// datasheet, "C+ mode" descriptor rings) and compiles and registers, but
// has never moved a packet. It only binds to the PCI ids below, so boot is
// unaffected when no such card is present.
//
// Known gaps compared with Linux's r8169: no per-revision PHY/EPHY setup or
// PHY firmware (some RTL8168 revisions may not get a link without it), no
// jumbo frames, checksum offload or TSO, no power management/ASPM tuning.
//
// Operation: C+ mode with one RX and one TX ring of 16-byte descriptors
// (OWN bit = owned by the chip, EOR marks the ring's last entry). An MSI
// interrupt wakes the RX thread, which also polls every few ms as a
// fallback, like the e1000 driver.

#include <arch/x86_64/interrupts.hpp>
#include <core/log.hpp>
#include <dev/device.hpp>
#include <drivers/pci/pci.hpp>
#include <lib/errno.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <net/net.hpp>
#include <sched/sched.hpp>
#include <sched/sync.hpp>
#include <time/clock.hpp>

namespace r8169 {

namespace {

// Registers (byte offsets into the MMIO BAR).
constexpr u32 MAC0 = 0x00, MAR0 = 0x08, TX_DESC_LO = 0x20, TX_DESC_HI = 0x24, CHIP_CMD = 0x37, TX_POLL = 0x38,
              INTR_MASK = 0x3C, INTR_STATUS = 0x3E, TX_CONFIG = 0x40, RX_CONFIG = 0x44, CFG9346 = 0x50,
              PHY_STATUS = 0x6C, RX_MAX_SIZE = 0xDA, CPLUS_CMD = 0xE0, INTR_MITIGATE = 0xE2, RX_DESC_LO = 0xE4,
              RX_DESC_HI = 0xE8, MAX_TX_PACKET = 0xEC;
constexpr u8 CMD_RESET = 0x10, CMD_RX_EN = 0x08, CMD_TX_EN = 0x04;
constexpr u8 POLL_NORMAL_QUEUE = 0x40;
constexpr u8 CFG9346_UNLOCK = 0xC0, CFG9346_LOCK = 0x00;
constexpr u16 INT_RX_OK = 0x01, INT_RX_ERR = 0x02, INT_TX_OK = 0x04, INT_TX_ERR = 0x08, INT_RX_OVERFLOW = 0x10,
              INT_LINK_CHANGE = 0x20, INT_RX_FIFO_OVER = 0x40;
constexpr u32 RX_ACCEPT_BROADCAST = 0x08, RX_ACCEPT_MULTICAST = 0x04, RX_ACCEPT_MY_PHYS = 0x02;
constexpr u32 RX_FIFO_NONE = 7u << 13, RX_DMA_UNLIMITED = 7u << 8;
constexpr u32 TX_IFG_NORMAL = 3u << 24, TX_DMA_UNLIMITED = 7u << 8;
constexpr u8 PHY_LINK = 0x02;

// Descriptor opts1 bits.
constexpr u32 DESC_OWN = 1u << 31, DESC_EOR = 1u << 30, DESC_FS = 1u << 29, DESC_LS = 1u << 28,
              RX_RES = 1u << 21; // RX error summary
constexpr u32 LEN_MASK = 0x3FFF;

constexpr u32 RX_COUNT = 128, TX_COUNT = 64, BUF_SIZE = 2048;

struct Desc {
    u32 opts1, opts2;
    u64 addr;
};
static_assert(sizeof(Desc) == 16);

class Nic : public net::Interface {
public:
    volatile u8* regs = nullptr;
    Desc* rx = nullptr;
    Desc* tx = nullptr;
    u8* rx_bufs[RX_COUNT] = {};
    u8* tx_bufs[TX_COUNT] = {};
    u32 rx_next = 0, tx_next = 0;
    IrqSpinlock tx_lock;
    sched::Semaphore irq_signal;

    u8 rd8(u32 r) { return *reinterpret_cast<volatile u8*>(regs + r); }
    u16 rd16(u32 r) { return *reinterpret_cast<volatile u16*>(regs + r); }
    u32 rd32(u32 r) { return *reinterpret_cast<volatile u32*>(regs + r); }
    void wr8(u32 r, u8 v) { *reinterpret_cast<volatile u8*>(regs + r) = v; }
    void wr16(u32 r, u16 v) { *reinterpret_cast<volatile u16*>(regs + r) = v; }
    void wr32(u32 r, u32 v) { *reinterpret_cast<volatile u32*>(regs + r) = v; }

    long transmit(const u8* frame, usize len) override {
        if (len > BUF_SIZE || len > LEN_MASK)
            return -err::INVAL;
        LockGuard guard(tx_lock);
        Desc& d = tx[tx_next];
        if (d.opts1 & DESC_OWN)
            return -err::AGAIN; // ring full: the chip still owns the next slot
        memcpy(tx_bufs[tx_next], frame, len);
        u32 eor = tx_next == TX_COUNT - 1 ? DESC_EOR : 0;
        d.opts2 = 0;
        __atomic_thread_fence(__ATOMIC_RELEASE); // buffer and opts2 before handing it over
        d.opts1 = DESC_OWN | eor | DESC_FS | DESC_LS | static_cast<u32>(len);
        tx_next = (tx_next + 1) % TX_COUNT;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        wr8(TX_POLL, POLL_NORMAL_QUEUE); // tell the chip to look at the ring
        return 0;
    }

    void give_rx(u32 i) {
        rx[i].opts2 = 0;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        rx[i].opts1 = DESC_OWN | (i == RX_COUNT - 1 ? DESC_EOR : 0) | BUF_SIZE;
    }

    bool alloc_rings() {
        u64 rx_phys = pmm::alloc_pages(pmm::order_for_size(RX_COUNT * sizeof(Desc)), true);
        u64 tx_phys = pmm::alloc_pages(pmm::order_for_size(TX_COUNT * sizeof(Desc)), true);
        if (!rx_phys || !tx_phys)
            return false;
        // Rings must be 256-byte aligned: pages are.
        rx = mm::phys_to_virt<Desc>(rx_phys);
        tx = mm::phys_to_virt<Desc>(tx_phys);
        for (u32 i = 0; i < RX_COUNT; i += 2) {
            u64 page = pmm::alloc_page(true);
            if (!page)
                return false;
            for (u32 k = 0; k < 2; k++) {
                rx_bufs[i + k] = mm::phys_to_virt<u8>(page + k * BUF_SIZE);
                rx[i + k].addr = page + k * BUF_SIZE;
                give_rx(i + k);
            }
        }
        for (u32 i = 0; i < TX_COUNT; i += 2) {
            u64 page = pmm::alloc_page(true);
            if (!page)
                return false;
            for (u32 k = 0; k < 2; k++) {
                tx_bufs[i + k] = mm::phys_to_virt<u8>(page + k * BUF_SIZE);
                tx[i + k].addr = page + k * BUF_SIZE;
                tx[i + k].opts1 = (i + k == TX_COUNT - 1) ? DESC_EOR : 0; // free (OWN clear)
                tx[i + k].opts2 = 0;
            }
        }
        wr32(TX_DESC_LO, static_cast<u32>(tx_phys));
        wr32(TX_DESC_HI, static_cast<u32>(tx_phys >> 32));
        wr32(RX_DESC_LO, static_cast<u32>(rx_phys));
        wr32(RX_DESC_HI, static_cast<u32>(rx_phys >> 32));
        return true;
    }

    bool start() {
        wr16(INTR_MASK, 0);
        wr8(CHIP_CMD, CMD_RESET);
        u64 deadline = clock::now_ns() + 100'000'000;
        while (rd8(CHIP_CMD) & CMD_RESET) {
            if (clock::now_ns() > deadline)
                return false;
            sched::sleep_ms(1);
        }
        // The MAC address was loaded from the EEPROM by the reset.
        for (int i = 0; i < 6; i++)
            mac[i] = rd8(MAC0 + i);

        wr8(CFG9346, CFG9346_UNLOCK); // configuration registers writable
        wr16(CPLUS_CMD, rd16(CPLUS_CMD) & static_cast<u16>(~0x60)); // no RX checksum/VLAN offload (C+ mode, plain)
        wr16(INTR_MITIGATE, 0);
        wr16(RX_MAX_SIZE, BUF_SIZE); // longer frames are dropped by the chip
        wr8(MAX_TX_PACKET, 0x3B);    // RTL8168: TX size limit in 128-byte units (ignored by older chips)
        if (!alloc_rings())
            return false;
        wr8(CHIP_CMD, CMD_RX_EN | CMD_TX_EN);
        // Some revisions only latch the configuration once the engines are on.
        wr32(TX_CONFIG, TX_IFG_NORMAL | TX_DMA_UNLIMITED);
        wr32(RX_CONFIG, RX_FIFO_NONE | RX_DMA_UNLIMITED | RX_ACCEPT_BROADCAST | RX_ACCEPT_MULTICAST |
                            RX_ACCEPT_MY_PHYS);
        // All multicast (IPv6 neighbour discovery needs it).
        wr32(MAR0, 0xFFFFFFFF);
        wr32(MAR0 + 4, 0xFFFFFFFF);
        wr8(CFG9346, CFG9346_LOCK);
        wr16(INTR_STATUS, 0xFFFF); // acknowledge anything pending
        link_up = rd8(PHY_STATUS) & PHY_LINK;
        return true;
    }

    void poll_rx() {
        for (;;) {
            Desc& d = rx[rx_next];
            u32 status = __atomic_load_n(&d.opts1, __ATOMIC_ACQUIRE);
            if (status & DESC_OWN)
                return;
            u32 len = status & LEN_MASK; // includes the 4-byte CRC
            bool whole = (status & DESC_FS) && (status & DESC_LS);
            if (whole && !(status & RX_RES) && len >= 14 + 4 && len <= BUF_SIZE)
                net::receive(this, rx_bufs[rx_next], len - 4);
            else
                rx_dropped++;
            give_rx(rx_next);
            rx_next = (rx_next + 1) % RX_COUNT;
        }
    }
};

Nic* g_irq_nics[8];
u32 g_irq_nic_count = 0;

void irq(interrupts::Frame*) {
    for (u32 i = 0; i < g_irq_nic_count; i++) {
        Nic* n = g_irq_nics[i];
        u16 status = n->rd16(INTR_STATUS);
        if (status && status != 0xFFFF) {
            n->wr16(INTR_STATUS, status); // write 1s to acknowledge
            n->irq_signal.signal();
        }
    }
}

void rx_thread(void* arg) {
    auto* n = static_cast<Nic*>(arg);
    for (;;) {
        n->irq_signal.wait_timeout(5'000'000);
        n->link_up = n->rd8(PHY_STATUS) & PHY_LINK;
        n->poll_rx();
    }
}

bool match(const dev::Device* d) {
    if (d->bus != dev::Bus::Pci)
        return false;
    if (d->vendor == 0x10EC) {
        switch (d->device) {
        case 0x8161: case 0x8168: case 0x8169: case 0x8167: // RTL8111/8168/8169 family
        case 0x8136:                                        // RTL8101/8102/8103 (Fast Ethernet)
        case 0x2502: case 0x2600:                           // RTL8111 variants on some boards
            return true;                                    // (RTL8125 2.5G, 0x8125, is a different design)
        default:
            return false;
        }
    }
    // Rebadged RTL8169/8168 cards (D-Link DGE-528T, TP-Link, US Robotics).
    return (d->vendor == 0x1186 && (d->device == 0x4300 || d->device == 0x4302)) ||
           (d->vendor == 0x1259 && d->device == 0xC107) || (d->vendor == 0x16EC && d->device == 0x0116);
}

long probe(dev::Device* d) {
    pci::Function* f = pci::function_of(d);
    pci::enable(*f);
    auto* n = new Nic;
    // The registers are in the first memory BAR: BAR 2 on RTL8168/8111 (64-bit), BAR 1 on RTL8169.
    n->regs = static_cast<volatile u8*>(pci::map_bar(*f, 2));
    if (!n->regs)
        n->regs = static_cast<volatile u8*>(pci::map_bar(*f, 1));
    if (!n->regs || !n->start()) {
        klog::warn("r8169: %04x:%04x did not initialize", d->vendor, d->device);
        return -err::IO;
    }
    n->hw = d;
    u32 xid = (n->rd32(TX_CONFIG) >> 20) & 0xFCF; // chip revision (Linux's XID)
    snprintf(d->name, sizeof(d->name), "Realtek RTL%s Ethernet (%04x, XID %03x)",
             d->device == 0x8136 ? "810x" : d->device == 0x8169 ? "8169" : "8111/8168", d->device, xid);
    u8 vector = interrupts::allocate_vector();
    if (g_irq_nic_count < 8 && pci::enable_msi(*f, vector)) {
        g_irq_nics[g_irq_nic_count++] = n;
        interrupts::set_handler(vector, irq);
        n->wr16(INTR_MASK, INT_RX_OK | INT_RX_ERR | INT_TX_OK | INT_TX_ERR | INT_RX_OVERFLOW | INT_LINK_CHANGE |
                               INT_RX_FIFO_OVER);
    }
    klog::warn("r8169: %s, MAC %02x:%02x:%02x:%02x:%02x:%02x: driver is untested on real hardware", d->name,
               n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
    net::register_interface(n);
    sched::spawn("r8169-rx", rx_thread, n, -1, true);
    return 0;
}

dev::Driver g_driver{"r8169", match, probe, {}};

} // namespace

void register_driver() { dev::register_driver(&g_driver); }

} // namespace r8169
