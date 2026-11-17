// AHCI (SATA) host controller driver.
//
// One command at a time per port, completion by polling, DMA through a
// per-port bounce buffer. Simple and robust; NCQ and interrupt-driven
// completion are later optimizations.

#include <block/block.hpp>
#include <core/log.hpp>
#include <dev/device.hpp>
#include <drivers/pci/pci.hpp>
#include <drivers/storage/storage.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <sched/sync.hpp>
#include <time/clock.hpp>

namespace ahci {

namespace {

// HBA registers
constexpr u32 HBA_CAP = 0x00, HBA_GHC = 0x04, HBA_PI = 0x0C, HBA_VS = 0x10;
constexpr u32 GHC_AE = 1u << 31;

// Port registers (offset from 0x100 + port * 0x80)
constexpr u32 PX_CLB = 0x00, PX_CLBU = 0x04, PX_FB = 0x08, PX_FBU = 0x0C, PX_IS = 0x10, PX_IE = 0x14,
              PX_CMD = 0x18, PX_TFD = 0x20, PX_SIG = 0x24, PX_SSTS = 0x28, PX_SERR = 0x30, PX_CI = 0x38;
constexpr u32 CMD_ST = 1 << 0, CMD_FRE = 1 << 4, CMD_FR = 1 << 14, CMD_CR = 1 << 15;
constexpr u32 TFD_ERR = 1 << 0, TFD_DRQ = 1 << 3, TFD_BSY = 1 << 7;
constexpr u32 IS_TFES = 1 << 30;
constexpr u32 SIG_ATA = 0x00000101;

constexpr u8 ATA_IDENTIFY = 0xEC, ATA_READ_DMA_EXT = 0x25, ATA_WRITE_DMA_EXT = 0x35, ATA_FLUSH_EXT = 0xEA;

constexpr usize BOUNCE_ORDER = 4;                     // 64 KiB
constexpr u32 MAX_SECTORS = (4096u << BOUNCE_ORDER) / 512;

struct PACKED CommandHeader {
    u16 flags;   // CFL (bits 0-4), A, W (bit 6), P, R, B, C, PMP
    u16 prdtl;   // PRD table entries
    u32 prdbc;   // bytes transferred
    u32 ctba;    // command table base (128-byte aligned)
    u32 ctbau;
    u32 reserved[4];
};

struct PACKED PrdEntry {
    u32 dba, dbau, reserved, dbc; // dbc: byte count - 1 (bits 0-21), bit 31: interrupt
};

struct PACKED CommandTable {
    u8 cfis[64];
    u8 acmd[16];
    u8 reserved[48];
    PrdEntry prdt[1];
};

class Port : public block::Device {
public:
    Port(volatile u8* hba, u32 index) : m_regs(hba + 0x100 + index * 0x80), m_index(index) {}

    bool start();

    long read_sectors(u64 lba, u32 count, void* buf) override { return transfer(lba, count, buf, false); }
    long write_sectors(u64 lba, u32 count, const void* buf) override {
        return transfer(lba, count, const_cast<void*>(buf), true);
    }
    long flush() override {
        LockGuard guard(m_lock);
        return issue(ATA_FLUSH_EXT, 0, 0, false, 0);
    }

private:
    u32 reg(u32 off) { return *reinterpret_cast<volatile u32*>(m_regs + off); }
    void set(u32 off, u32 v) { *reinterpret_cast<volatile u32*>(m_regs + off) = v; }

    bool wait_clear(u32 off, u32 bits, u64 timeout_ms) {
        u64 deadline = clock::now_ns() + timeout_ms * 1'000'000;
        while (reg(off) & bits)
            if (clock::now_ns() > deadline)
                return false;
        return true;
    }

    long issue(u8 command, u64 lba, u32 count, bool write, usize bytes);
    long transfer(u64 lba, u32 count, void* buf, bool write);

    volatile u8* m_regs;
    u32 m_index;
    CommandHeader* m_cmd_list = nullptr;
    CommandTable* m_table = nullptr;
    u64 m_table_phys = 0;
    u8* m_bounce = nullptr;
    u64 m_bounce_phys = 0;
    sched::Mutex m_lock;
};

bool Port::start() {
    // Stop the command engine before reprogramming it.
    set(PX_CMD, reg(PX_CMD) & ~CMD_ST);
    if (!wait_clear(PX_CMD, CMD_CR, 500))
        return false;
    set(PX_CMD, reg(PX_CMD) & ~CMD_FRE);
    if (!wait_clear(PX_CMD, CMD_FR, 500))
        return false;

    // One page: command list (1 KiB) + received-FIS area (256 B) + one command table.
    u64 page = pmm::alloc_page(true);
    u64 bounce = pmm::alloc_pages(BOUNCE_ORDER);
    if (!page || !bounce)
        return false;
    m_cmd_list = mm::phys_to_virt<CommandHeader>(page);
    u64 fis_phys = page + 1024;
    m_table_phys = page + 2048; // 128-byte aligned
    m_table = mm::phys_to_virt<CommandTable>(m_table_phys);
    m_bounce = mm::phys_to_virt<u8>(bounce);
    m_bounce_phys = bounce;

    set(PX_CLB, static_cast<u32>(page));
    set(PX_CLBU, static_cast<u32>(page >> 32));
    set(PX_FB, static_cast<u32>(fis_phys));
    set(PX_FBU, static_cast<u32>(fis_phys >> 32));
    set(PX_SERR, 0xFFFFFFFF);
    set(PX_IS, 0xFFFFFFFF);
    set(PX_IE, 0); // polled
    set(PX_CMD, reg(PX_CMD) | CMD_FRE);
    set(PX_CMD, reg(PX_CMD) | CMD_ST);

    // IDENTIFY DEVICE: model name and capacity.
    if (issue(ATA_IDENTIFY, 0, 0, false, 512) != 0)
        return false;
    auto* id = reinterpret_cast<const u16*>(m_bounce);
    u64 lba48 = id[100] | (static_cast<u64>(id[101]) << 16) | (static_cast<u64>(id[102]) << 32) |
                (static_cast<u64>(id[103]) << 48);
    u64 lba28 = id[60] | (static_cast<u64>(id[61]) << 16);
    sector_count = (id[83] & (1 << 10)) && lba48 ? lba48 : lba28;
    sector_size = 512;
    for (int i = 0; i < 20; i++) { // model: words 27-46, bytes swapped
        model[i * 2] = static_cast<char>(id[27 + i] >> 8);
        model[i * 2 + 1] = static_cast<char>(id[27 + i] & 0xFF);
    }
    model[40] = 0;
    for (int i = 39; i >= 0 && model[i] == ' '; i--)
        model[i] = 0;
    return sector_count > 0;
}

long Port::issue(u8 command, u64 lba, u32 count, bool write, usize bytes) {
    if (!wait_clear(PX_TFD, TFD_BSY | TFD_DRQ, 1000))
        return -err::IO;

    CommandHeader& h = m_cmd_list[0];
    memset(&h, 0, sizeof(h));
    h.flags = 5 | (write ? (1 << 6) : 0); // FIS length: 5 dwords
    h.prdtl = bytes ? 1 : 0;
    h.ctba = static_cast<u32>(m_table_phys);
    h.ctbau = static_cast<u32>(m_table_phys >> 32);

    memset(m_table, 0, sizeof(CommandTable));
    u8* fis = m_table->cfis;
    fis[0] = 0x27;    // FIS type: register, host to device
    fis[1] = 0x80;    // this is a command
    fis[2] = command;
    fis[4] = static_cast<u8>(lba);
    fis[5] = static_cast<u8>(lba >> 8);
    fis[6] = static_cast<u8>(lba >> 16);
    fis[7] = 0x40;    // LBA mode
    fis[8] = static_cast<u8>(lba >> 24);
    fis[9] = static_cast<u8>(lba >> 32);
    fis[10] = static_cast<u8>(lba >> 40);
    fis[12] = static_cast<u8>(count);
    fis[13] = static_cast<u8>(count >> 8);
    if (bytes) {
        m_table->prdt[0].dba = static_cast<u32>(m_bounce_phys);
        m_table->prdt[0].dbau = static_cast<u32>(m_bounce_phys >> 32);
        m_table->prdt[0].dbc = static_cast<u32>(bytes - 1);
    }

    set(PX_IS, 0xFFFFFFFF);
    set(PX_CI, 1); // go: command slot 0
    u64 deadline = clock::now_ns() + 5'000'000'000ull;
    while (reg(PX_CI) & 1) {
        if ((reg(PX_IS) & IS_TFES) || clock::now_ns() > deadline) {
            klog::error("ahci: port %u command %#x failed (TFD %#x, IS %#x)", m_index, command, reg(PX_TFD),
                        reg(PX_IS));
            return -err::IO;
        }
    }
    if (reg(PX_TFD) & TFD_ERR)
        return -err::IO;
    return 0;
}

long Port::transfer(u64 lba, u32 count, void* buf, bool write) {
    if (lba + count > sector_count)
        return -err::IO;
    LockGuard guard(m_lock);
    auto* p = static_cast<u8*>(buf);
    while (count) {
        u32 n = min(count, MAX_SECTORS);
        usize bytes = static_cast<usize>(n) * 512;
        if (write)
            memcpy(m_bounce, p, bytes);
        if (long r = issue(write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, lba, n, write, bytes); r != 0)
            return r;
        if (!write)
            memcpy(p, m_bounce, bytes);
        p += bytes;
        lba += n;
        count -= n;
    }
    return 0;
}

bool match(const dev::Device* d) { return d->bus == dev::Bus::Pci && (d->class_code >> 8) == 0x0106 && (d->class_code & 0xFF) == 1; }

long probe(dev::Device* d) {
    pci::Function* f = pci::function_of(d);
    if (!f->bars[5].size)
        return -err::NODEV;
    pci::enable(*f);
    auto* hba = static_cast<volatile u8*>(pci::map_bar(*f, 5));
    if (!hba)
        return -err::NOMEM;
    auto r32 = [&](u32 off) { return *reinterpret_cast<volatile u32*>(hba + off); };
    *reinterpret_cast<volatile u32*>(hba + HBA_GHC) = r32(HBA_GHC) | GHC_AE;

    u32 implemented = r32(HBA_PI);
    u32 vs = r32(HBA_VS);
    int disks = 0;
    for (u32 i = 0; i < 32; i++) {
        if (!(implemented & (1u << i)))
            continue;
        volatile u8* pregs = hba + 0x100 + i * 0x80;
        u32 ssts = *reinterpret_cast<volatile u32*>(pregs + PX_SSTS);
        u32 sig = *reinterpret_cast<volatile u32*>(pregs + PX_SIG);
        if ((ssts & 0xF) != 3 || sig != SIG_ATA)
            continue; // no device, or not a disk (ATAPI optical drives later)
        auto* port = new Port(hba, i);
        if (!port->start()) {
            klog::warn("ahci: port %u: disk did not respond", i);
            delete port;
            continue;
        }
        block::next_sd_name(port->name, sizeof(port->name));
        port->hw = d;
        block::register_disk(port);
        disks++;
    }
    snprintf(d->name, sizeof(d->name), "SATA AHCI controller (AHCI %u.%u, %d disk%s)", vs >> 16, (vs >> 8) & 0xFF,
             disks, disks == 1 ? "" : "s");
    (void)HBA_CAP;
    return 0;
}

dev::Driver g_driver{"ahci", match, probe, {}};

} // namespace

void register_driver() { dev::register_driver(&g_driver); }

} // namespace ahci
