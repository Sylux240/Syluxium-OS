// NVM Express driver (PCIe SSDs).
//
// Admin queue + one I/O queue pair, completion by polling, DMA through a
// bounce buffer described by a PRP list. Interrupts and per-CPU queues are
// later optimizations.

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

namespace nvme {

namespace {

constexpr u32 REG_CAP = 0x00, REG_VS = 0x08, REG_CC = 0x14, REG_CSTS = 0x1C, REG_AQA = 0x24, REG_ASQ = 0x28,
              REG_ACQ = 0x30;
constexpr u32 QUEUE_ENTRIES = 64;
constexpr usize BOUNCE_ORDER = 4; // 64 KiB per I/O

struct PACKED Command {
    u32 cdw0; // opcode | cid << 16
    u32 nsid;
    u64 reserved;
    u64 mptr;
    u64 prp1, prp2;
    u32 cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};
static_assert(sizeof(Command) == 64);

struct PACKED Completion {
    u32 result;
    u32 reserved;
    u16 sq_head, sq_id;
    u16 cid;
    u16 status; // bit 0: phase tag
};
static_assert(sizeof(Completion) == 16);

struct Queue {
    Command* sq = nullptr;
    Completion* cq = nullptr;
    u64 sq_phys = 0, cq_phys = 0;
    u16 sq_tail = 0, cq_head = 0;
    u8 phase = 1;
    volatile u32* sq_doorbell = nullptr;
    volatile u32* cq_doorbell = nullptr;
};

class Controller;

class Namespace : public block::Device {
public:
    Namespace(Controller* c, u32 nsid) : m_ctrl(c), m_nsid(nsid) {}
    long read_sectors(u64 lba, u32 count, void* buf) override;
    long write_sectors(u64 lba, u32 count, const void* buf) override;
    long flush() override;

private:
    long transfer(u64 lba, u32 count, void* buf, bool write);
    Controller* m_ctrl;
    u32 m_nsid;
};

class Controller {
public:
    volatile u8* regs = nullptr;
    u32 doorbell_stride = 4;
    Queue admin, io;
    u16 next_cid = 0;
    u8* bounce = nullptr;
    u64 bounce_phys = 0;
    u64* prp_list = nullptr;
    u64 prp_list_phys = 0;
    u32 max_transfer = 4096u << BOUNCE_ORDER;
    sched::Mutex lock;

    u32 r32(u32 off) { return *reinterpret_cast<volatile u32*>(regs + off); }
    u64 r64(u32 off) { return *reinterpret_cast<volatile u64*>(regs + off); }
    void w32(u32 off, u32 v) { *reinterpret_cast<volatile u32*>(regs + off) = v; }
    void w64(u32 off, u64 v) { *reinterpret_cast<volatile u64*>(regs + off) = v; }

    bool alloc_queue(Queue& q, u16 qid) {
        q.sq_phys = pmm::alloc_page(true);
        q.cq_phys = pmm::alloc_page(true);
        if (!q.sq_phys || !q.cq_phys)
            return false;
        q.sq = mm::phys_to_virt<Command>(q.sq_phys);
        q.cq = mm::phys_to_virt<Completion>(q.cq_phys);
        q.sq_doorbell = reinterpret_cast<volatile u32*>(regs + 0x1000 + (2 * qid) * doorbell_stride);
        q.cq_doorbell = reinterpret_cast<volatile u32*>(regs + 0x1000 + (2 * qid + 1) * doorbell_stride);
        return true;
    }

    // Submit and wait for completion. Returns the NVMe status (0 = success) or -1 on timeout.
    int submit(Queue& q, Command cmd, u32* result = nullptr) {
        u16 cid = next_cid++;
        cmd.cdw0 = (cmd.cdw0 & 0xFFFF) | (static_cast<u32>(cid) << 16);
        q.sq[q.sq_tail] = cmd;
        q.sq_tail = static_cast<u16>((q.sq_tail + 1) % QUEUE_ENTRIES);
        *q.sq_doorbell = q.sq_tail;

        u64 deadline = clock::now_ns() + 5'000'000'000ull;
        for (;;) {
            volatile Completion& c = q.cq[q.cq_head];
            if ((c.status & 1) == q.phase) {
                u16 status = c.status >> 1;
                u32 res = c.result;
                q.cq_head = static_cast<u16>((q.cq_head + 1) % QUEUE_ENTRIES);
                if (q.cq_head == 0)
                    q.phase ^= 1;
                *q.cq_doorbell = q.cq_head;
                if (result)
                    *result = res;
                return status;
            }
            if (clock::now_ns() > deadline)
                return -1;
        }
    }

    // Fill PRP1/PRP2 for `bytes` of the bounce buffer.
    void set_prps(Command& cmd, usize bytes) {
        cmd.prp1 = bounce_phys;
        usize pages = (bytes + 4095) / 4096;
        if (pages <= 1) {
            cmd.prp2 = 0;
        } else if (pages == 2) {
            cmd.prp2 = bounce_phys + 4096;
        } else {
            for (usize i = 1; i < pages; i++)
                prp_list[i - 1] = bounce_phys + i * 4096;
            cmd.prp2 = prp_list_phys;
        }
    }
};

long Namespace::transfer(u64 lba, u32 count, void* buf, bool write) {
    if (lba + count > sector_count)
        return -err::IO;
    Controller* c = m_ctrl;
    LockGuard guard(c->lock);
    auto* p = static_cast<u8*>(buf);
    u32 max_sectors = c->max_transfer / sector_size;
    while (count) {
        u32 n = min(count, max_sectors);
        usize bytes = static_cast<usize>(n) * sector_size;
        if (write)
            memcpy(c->bounce, p, bytes);
        Command cmd{};
        cmd.cdw0 = write ? 0x01 : 0x02;
        cmd.nsid = m_nsid;
        c->set_prps(cmd, bytes);
        cmd.cdw10 = static_cast<u32>(lba);
        cmd.cdw11 = static_cast<u32>(lba >> 32);
        cmd.cdw12 = n - 1; // 0-based count
        int status = c->submit(c->io, cmd);
        if (status != 0) {
            klog::error("nvme: %s of %u sectors at %lu failed (status %#x)", write ? "write" : "read", n, lba, status);
            return -err::IO;
        }
        if (!write)
            memcpy(p, c->bounce, bytes);
        p += bytes;
        lba += n;
        count -= n;
    }
    return 0;
}

long Namespace::read_sectors(u64 lba, u32 count, void* buf) { return transfer(lba, count, buf, false); }
long Namespace::write_sectors(u64 lba, u32 count, const void* buf) {
    return transfer(lba, count, const_cast<void*>(buf), true);
}

long Namespace::flush() {
    LockGuard guard(m_ctrl->lock);
    Command cmd{};
    cmd.cdw0 = 0x00;
    cmd.nsid = m_nsid;
    return m_ctrl->submit(m_ctrl->io, cmd) == 0 ? 0 : -err::IO;
}

int g_controllers = 0;

bool match(const dev::Device* d) { return d->bus == dev::Bus::Pci && d->class_code == 0x010802; }

long probe(dev::Device* d) {
    pci::Function* f = pci::function_of(d);
    pci::enable(*f);
    auto* c = new Controller;
    c->regs = static_cast<volatile u8*>(pci::map_bar(*f, 0));
    if (!c->regs)
        return -err::NOMEM;

    u64 cap = c->r64(REG_CAP);
    c->doorbell_stride = 4u << ((cap >> 32) & 0xF);
    u64 timeout_ms = ((cap >> 24) & 0xFF) * 500 + 1000;

    // Reset: disable, wait until not ready.
    c->w32(REG_CC, c->r32(REG_CC) & ~1u);
    u64 deadline = clock::now_ns() + timeout_ms * 1'000'000;
    while (c->r32(REG_CSTS) & 1)
        if (clock::now_ns() > deadline)
            return -err::TIMEDOUT;

    if (!c->alloc_queue(c->admin, 0) || !c->alloc_queue(c->io, 1))
        return -err::NOMEM;
    c->w32(REG_AQA, ((QUEUE_ENTRIES - 1) << 16) | (QUEUE_ENTRIES - 1));
    c->w64(REG_ASQ, c->admin.sq_phys);
    c->w64(REG_ACQ, c->admin.cq_phys);
    // Enable: NVM command set, 4 KiB pages, 64-byte SQ / 16-byte CQ entries.
    c->w32(REG_CC, 1 | (6u << 16) | (4u << 20));
    deadline = clock::now_ns() + timeout_ms * 1'000'000;
    while (!(c->r32(REG_CSTS) & 1)) {
        if (c->r32(REG_CSTS) & 2)
            return -err::IO; // controller fatal status
        if (clock::now_ns() > deadline)
            return -err::TIMEDOUT;
    }

    u64 bounce = pmm::alloc_pages(BOUNCE_ORDER);
    u64 prp = pmm::alloc_page(true);
    if (!bounce || !prp)
        return -err::NOMEM;
    c->bounce = mm::phys_to_virt<u8>(bounce);
    c->bounce_phys = bounce;
    c->prp_list = mm::phys_to_virt<u64>(prp);
    c->prp_list_phys = prp;

    // Identify controller (CNS 1).
    Command id{};
    id.cdw0 = 0x06;
    c->set_prps(id, 4096);
    id.cdw10 = 1;
    if (c->submit(c->admin, id) != 0)
        return -err::IO;
    char model[41] = {};
    memcpy(model, c->bounce + 24, 40);
    for (int i = 39; i >= 0 && (model[i] == ' ' || model[i] == 0); i--)
        model[i] = 0;
    u8 mdts = c->bounce[77]; // max transfer: 2^mdts pages (0 = unlimited)
    if (mdts && (4096u << mdts) < c->max_transfer)
        c->max_transfer = 4096u << mdts;

    // I/O completion queue 1, then submission queue 1 (polled: no interrupts).
    Command cq{};
    cq.cdw0 = 0x05;
    cq.prp1 = c->io.cq_phys;
    cq.cdw10 = ((QUEUE_ENTRIES - 1) << 16) | 1;
    cq.cdw11 = 1; // physically contiguous
    if (c->submit(c->admin, cq) != 0)
        return -err::IO;
    Command sq{};
    sq.cdw0 = 0x01;
    sq.prp1 = c->io.sq_phys;
    sq.cdw10 = ((QUEUE_ENTRIES - 1) << 16) | 1;
    sq.cdw11 = (1u << 16) | 1; // completion queue 1, contiguous
    if (c->submit(c->admin, sq) != 0)
        return -err::IO;

    // Namespace 1 (the common single-namespace case).
    Command ns{};
    ns.cdw0 = 0x06;
    ns.nsid = 1;
    c->set_prps(ns, 4096);
    ns.cdw10 = 0;
    if (c->submit(c->admin, ns) != 0)
        return -err::IO;
    u64 nsze;
    memcpy(&nsze, c->bounce, 8);
    u8 flbas = c->bounce[26] & 0xF;
    u8 lbads = c->bounce[128 + flbas * 4 + 2];
    if (!nsze || lbads < 9)
        return -err::NODEV;

    auto* n = new Namespace(c, 1);
    snprintf(n->name, sizeof(n->name), "nvme%dn1", g_controllers);
    snprintf(n->model, sizeof(n->model), "%s", model);
    n->sector_count = nsze;
    n->sector_size = 1u << lbads;
    n->hw = d;
    u32 vs = c->r32(REG_VS);
    snprintf(d->name, sizeof(d->name), "NVMe SSD controller (NVMe %u.%u)", vs >> 16, (vs >> 8) & 0xFF);
    g_controllers++;
    block::register_disk(n);
    return 0;
}

dev::Driver g_driver{"nvme", match, probe, {}};

} // namespace

void register_driver() { dev::register_driver(&g_driver); }

} // namespace nvme
