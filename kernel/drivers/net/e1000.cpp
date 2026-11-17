// Intel e1000 / e1000e Gigabit Ethernet (82540EM, 82545EM, 82574L, I217,
// I218, I219): QEMU's default cards, and very common on real PCs.
//
// Legacy descriptors, one RX and one TX ring. An MSI interrupt wakes the
// RX thread; it also polls every few milliseconds as a fallback.

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

namespace e1000 {

namespace {

constexpr u32 CTRL = 0x0000, STATUS = 0x0008, EERD = 0x0014, ICR = 0x00C0, IMS = 0x00D0, IMC = 0x00D8,
              RCTL = 0x0100, TCTL = 0x0400, TIPG = 0x0410, RDBAL = 0x2800, RDBAH = 0x2804, RDLEN = 0x2808,
              RDH = 0x2810, RDT = 0x2818, TDBAL = 0x3800, TDBAH = 0x3804, TDLEN = 0x3808, TDH = 0x3810,
              TDT = 0x3818, MTA = 0x5200, RAL0 = 0x5400, RAH0 = 0x5404;
constexpr u32 CTRL_SLU = 1u << 6, CTRL_ASDE = 1u << 5, CTRL_RST = 1u << 26;
constexpr u32 RCTL_EN = 1u << 1, RCTL_BAM = 1u << 15, RCTL_SECRC = 1u << 26;
constexpr u32 TCTL_EN = 1u << 1, TCTL_PSP = 1u << 3;
constexpr u8 TX_EOP = 1, TX_IFCS = 2, TX_RS = 8;
constexpr u8 DESC_DD = 1, RX_EOP = 2;

constexpr u32 RX_COUNT = 128, TX_COUNT = 64, BUF_SIZE = 2048;

struct RxDesc {
    u64 addr;
    u16 length, checksum;
    u8 status, errors;
    u16 special;
};
struct TxDesc {
    u64 addr;
    u16 length;
    u8 cso, cmd, status, css;
    u16 special;
};
static_assert(sizeof(RxDesc) == 16 && sizeof(TxDesc) == 16);

class Nic : public net::Interface {
public:
    volatile u8* regs = nullptr;
    RxDesc* rx = nullptr;
    TxDesc* tx = nullptr;
    u8* rx_bufs[RX_COUNT] = {};
    u8* tx_bufs[TX_COUNT] = {};
    u32 rx_next = 0, tx_next = 0;
    IrqSpinlock tx_lock;
    sched::Semaphore irq_signal;

    u32 rd(u32 r) { return *reinterpret_cast<volatile u32*>(regs + r); }
    void wr(u32 r, u32 v) { *reinterpret_cast<volatile u32*>(regs + r) = v; }

    long transmit(const u8* frame, usize len) override {
        if (len > BUF_SIZE)
            return -err::INVAL;
        LockGuard guard(tx_lock);
        TxDesc& d = tx[tx_next];
        if (!(d.status & DESC_DD))
            return -err::AGAIN; // ring full
        memcpy(tx_bufs[tx_next], frame, len);
        d.length = static_cast<u16>(len);
        d.cmd = TX_EOP | TX_IFCS | TX_RS;
        d.status = 0;
        tx_next = (tx_next + 1) % TX_COUNT;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        wr(TDT, tx_next);
        return 0;
    }

    u16 eeprom_read(u8 addr, bool e1000e) {
        // 82540-style: start bit 0, address << 8, done bit 4. 82574+: address << 2, done bit 1.
        wr(EERD, e1000e ? (1u | (static_cast<u32>(addr) << 2)) : (1u | (static_cast<u32>(addr) << 8)));
        u32 done = e1000e ? (1u << 1) : (1u << 4);
        u64 deadline = clock::now_ns() + 10'000'000;
        u32 v;
        while (!((v = rd(EERD)) & done))
            if (clock::now_ns() > deadline)
                return 0;
        return static_cast<u16>(v >> 16);
    }

    bool start(bool e1000e) {
        wr(IMC, 0xFFFFFFFF);
        wr(CTRL, rd(CTRL) | CTRL_RST);
        sched::sleep_ms(10);
        u64 deadline = clock::now_ns() + 100'000'000;
        while (rd(CTRL) & CTRL_RST)
            if (clock::now_ns() > deadline)
                return false;
        wr(IMC, 0xFFFFFFFF);
        rd(ICR);
        wr(CTRL, (rd(CTRL) | CTRL_SLU | CTRL_ASDE) & ~(1u << 3) & ~(1u << 31)); // link up, no reset of PHY/LRST

        // MAC: from the receive-address register if the firmware set it, else the EEPROM.
        u32 ral = rd(RAL0), rah = rd(RAH0);
        if (rah & (1u << 31)) {
            for (int i = 0; i < 4; i++) mac[i] = static_cast<u8>(ral >> (8 * i));
            mac[4] = static_cast<u8>(rah);
            mac[5] = static_cast<u8>(rah >> 8);
        } else {
            for (u8 i = 0; i < 3; i++) {
                u16 w = eeprom_read(i, e1000e);
                mac[i * 2] = static_cast<u8>(w);
                mac[i * 2 + 1] = static_cast<u8>(w >> 8);
            }
            wr(RAL0, mac[0] | (mac[1] << 8) | (mac[2] << 16) | (static_cast<u32>(mac[3]) << 24));
            wr(RAH0, mac[4] | (mac[5] << 8) | (1u << 31));
        }
        for (u32 i = 0; i < 128; i++)
            wr(MTA + i * 4, 0);

        // Rings (one page each) and 2 KiB buffers (two per page).
        u64 rx_phys = pmm::alloc_pages(pmm::order_for_size(RX_COUNT * sizeof(RxDesc)), true);
        u64 tx_phys = pmm::alloc_page(true);
        if (!rx_phys || !tx_phys)
            return false;
        rx = mm::phys_to_virt<RxDesc>(rx_phys);
        tx = mm::phys_to_virt<TxDesc>(tx_phys);
        for (u32 i = 0; i < RX_COUNT; i += 2) {
            u64 page = pmm::alloc_page(true);
            if (!page)
                return false;
            for (u32 k = 0; k < 2; k++) {
                rx_bufs[i + k] = mm::phys_to_virt<u8>(page + k * BUF_SIZE);
                rx[i + k].addr = page + k * BUF_SIZE;
            }
        }
        for (u32 i = 0; i < TX_COUNT; i += 2) {
            u64 page = pmm::alloc_page(true);
            if (!page)
                return false;
            for (u32 k = 0; k < 2; k++) {
                tx_bufs[i + k] = mm::phys_to_virt<u8>(page + k * BUF_SIZE);
                tx[i + k].addr = page + k * BUF_SIZE;
                tx[i + k].status = DESC_DD; // free
            }
        }
        wr(RDBAL, static_cast<u32>(rx_phys));
        wr(RDBAH, static_cast<u32>(rx_phys >> 32));
        wr(RDLEN, RX_COUNT * sizeof(RxDesc));
        wr(RDH, 0);
        wr(RDT, RX_COUNT - 1);
        wr(RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC); // 2 KiB buffers (BSIZE 0)

        wr(TDBAL, static_cast<u32>(tx_phys));
        wr(TDBAH, static_cast<u32>(tx_phys >> 32));
        wr(TDLEN, TX_COUNT * sizeof(TxDesc));
        wr(TDH, 0);
        wr(TDT, 0);
        wr(TIPG, 0x0060200A);
        wr(TCTL, TCTL_EN | TCTL_PSP | (0x0F << 4) | (0x40 << 12));

        link_up = rd(STATUS) & 2;
        return true;
    }

    void poll_rx() {
        for (;;) {
            RxDesc& d = rx[rx_next];
            if (!(d.status & DESC_DD))
                return;
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if ((d.status & RX_EOP) && !d.errors && d.length >= 14)
                net::receive(this, rx_bufs[rx_next], d.length);
            else
                rx_dropped++;
            d.status = 0;
            u32 done = rx_next;
            rx_next = (rx_next + 1) % RX_COUNT;
            wr(RDT, done); // give the buffer back
        }
    }
};

Nic* g_irq_nics[8];
u32 g_irq_nic_count = 0;

void irq(interrupts::Frame*) {
    for (u32 i = 0; i < g_irq_nic_count; i++) {
        Nic* n = g_irq_nics[i];
        if (n->rd(ICR)) // reading clears it
            n->irq_signal.signal();
    }
}

void rx_thread(void* arg) {
    auto* n = static_cast<Nic*>(arg);
    for (;;) {
        n->irq_signal.wait_timeout(5'000'000);
        n->link_up = n->rd(STATUS) & 2;
        n->poll_rx();
    }
}

bool match(const dev::Device* d) {
    if (d->bus != dev::Bus::Pci || d->vendor != 0x8086 || (d->class_code >> 8) != 0x0200)
        return false;
    switch (d->device) {
    case 0x100E: case 0x100F: case 0x1004: case 0x1015: case 0x1011: // 82540EM, 82545EM, ...
    case 0x10D3: case 0x10F6:                                         // 82574L
    case 0x153A: case 0x153B: case 0x155A: case 0x1559:               // I217, I218
    case 0x15A0: case 0x15A1: case 0x15A2: case 0x15A3:               // I218
    case 0x156F: case 0x1570: case 0x15B7: case 0x15B8: case 0x15B9:  // I219
    case 0x15D7: case 0x15D8: case 0x15E3: case 0x15D6: case 0x15BD: case 0x15BE: case 0x15BB: case 0x15BC:
    case 0x15DF: case 0x15E0: case 0x15E1: case 0x15E2: case 0x0D4E: case 0x0D4F: case 0x0D4C: case 0x0D4D:
    case 0x15FB: case 0x15FC: case 0x15F9: case 0x15FA: case 0x1A1E: case 0x1A1F: case 0x1A1C: case 0x1A1D:
        return true;
    default:
        return false;
    }
}

long probe(dev::Device* d) {
    pci::Function* f = pci::function_of(d);
    pci::enable(*f);
    auto* n = new Nic;
    n->regs = static_cast<volatile u8*>(pci::map_bar(*f, 0));
    bool e1000e = d->device != 0x100E && d->device != 0x100F && d->device != 0x1004 && d->device != 0x1015 &&
                  d->device != 0x1011;
    if (!n->regs || !n->start(e1000e))
        return -err::IO;
    n->hw = d;
    snprintf(d->name, sizeof(d->name), "Intel %s Gigabit Ethernet (%04x)",
             d->device == 0x10D3 ? "82574L" : e1000e ? "I21x" : "PRO/1000", d->device);
    u8 vector = interrupts::allocate_vector();
    if (g_irq_nic_count < 8 && pci::enable_msi(*f, vector)) {
        g_irq_nics[g_irq_nic_count++] = n;
        interrupts::set_handler(vector, irq);
        n->wr(IMS, (1u << 7) | (1u << 4) | (1u << 2)); // RX timer, RX overrun-ish, link change
    }
    net::register_interface(n);
    sched::spawn("e1000-rx", rx_thread, n, -1, true);
    return 0;
}

dev::Driver g_driver{"e1000", match, probe, {}};

} // namespace

void register_driver() { dev::register_driver(&g_driver); }

} // namespace e1000
