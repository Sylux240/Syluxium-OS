// virtio-net (virtio 1.0 "modern" PCI interface): the paravirtual network
// card of QEMU/KVM and most clouds.

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

namespace virtio_net {

namespace {

constexpr u32 STATUS_ACK = 1, STATUS_DRIVER = 2, STATUS_DRIVER_OK = 4, STATUS_FEATURES_OK = 8;
constexpr u32 F_MAC = 5, F_STATUS = 16;      // feature bits (low word)
constexpr u32 F_VERSION_1 = 32;              // high word bit 0
constexpr u16 DESC_WRITE = 2;
constexpr usize HDR = 12;                    // struct virtio_net_hdr (with num_buffers)
constexpr u16 MAX_QUEUE = 128;
constexpr usize BUF_SIZE = 2048;

struct Desc {
    u64 addr;
    u32 len;
    u16 flags, next;
};

struct Queue {
    u16 size = 0;
    Desc* desc = nullptr;
    volatile u16* avail = nullptr; // flags, idx, ring[size]
    volatile u8* used = nullptr;   // flags, idx, ring[size] of {u32 id, u32 len}
    u16 avail_idx = 0, used_seen = 0;
    volatile u16* notify = nullptr;
    u16 index = 0;
    u8* bufs[MAX_QUEUE] = {};
    u64 buf_phys[MAX_QUEUE] = {};

    u16 used_idx() const { return *reinterpret_cast<volatile u16*>(used + 2); }
    void used_elem(u16 i, u32* id, u32* len) const {
        auto* e = reinterpret_cast<volatile u32*>(used + 4 + (i % size) * 8);
        *id = e[0];
        *len = e[1];
    }
    void offer(u16 id) {
        avail[2 + avail_idx % size] = id;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        avail[1] = ++avail_idx;
    }
    void kick() { *notify = index; }
};

class Nic : public net::Interface {
public:
    volatile u8* common = nullptr;
    volatile u8* device_cfg = nullptr;
    volatile u8* isr = nullptr;
    volatile u8* notify_base = nullptr;
    u32 notify_mult = 0;
    Queue rxq, txq;
    u16 tx_free_head = 0;
    bool tx_busy[MAX_QUEUE] = {};
    IrqSpinlock tx_lock;
    sched::Semaphore irq_signal;

    template <typename T> T c_rd(u32 off) { return *reinterpret_cast<volatile T*>(common + off); }
    template <typename T> void c_wr(u32 off, T v) { *reinterpret_cast<volatile T*>(common + off) = v; }

    bool setup_queue(Queue& q, u16 index, bool rx) {
        c_wr<u16>(22, index); // queue_select
        u16 size = c_rd<u16>(24);
        if (!size)
            return false;
        if (size > MAX_QUEUE)
            size = MAX_QUEUE;
        c_wr<u16>(24, size);
        q.size = size;
        q.index = index;
        u64 d = pmm::alloc_page(true), a = pmm::alloc_page(true), u = pmm::alloc_page(true);
        if (!d || !a || !u)
            return false;
        q.desc = mm::phys_to_virt<Desc>(d);
        q.avail = mm::phys_to_virt<u16>(a);
        q.used = mm::phys_to_virt<u8>(u);
        for (u16 i = 0; i < size; i += 2) {
            u64 page = pmm::alloc_page(true);
            if (!page)
                return false;
            for (u16 k = 0; k < 2 && i + k < size; k++) {
                q.buf_phys[i + k] = page + k * BUF_SIZE;
                q.bufs[i + k] = mm::phys_to_virt<u8>(page + k * BUF_SIZE);
                q.desc[i + k] = Desc{page + k * BUF_SIZE, BUF_SIZE, static_cast<u16>(rx ? DESC_WRITE : 0), 0};
            }
        }
        c_wr<u64>(32, d);
        c_wr<u64>(40, a);
        c_wr<u64>(48, u);
        u16 notify_off = c_rd<u16>(30);
        q.notify = reinterpret_cast<volatile u16*>(notify_base + notify_off * notify_mult);
        c_wr<u16>(28, 1); // queue_enable
        return true;
    }

    long transmit(const u8* frame, usize len) override {
        if (len + HDR > BUF_SIZE)
            return -err::INVAL;
        LockGuard guard(tx_lock);
        reclaim_tx();
        u16 id = MAX_QUEUE;
        for (u16 i = 0; i < txq.size; i++) {
            u16 c = static_cast<u16>((tx_free_head + i) % txq.size);
            if (!tx_busy[c]) {
                id = c;
                break;
            }
        }
        if (id == MAX_QUEUE)
            return -err::AGAIN;
        tx_free_head = static_cast<u16>((id + 1) % txq.size);
        memset(txq.bufs[id], 0, HDR);
        memcpy(txq.bufs[id] + HDR, frame, len);
        txq.desc[id].len = static_cast<u32>(len + HDR);
        tx_busy[id] = true;
        txq.offer(id);
        txq.kick();
        return 0;
    }

    void reclaim_tx() {
        while (txq.used_seen != txq.used_idx()) {
            u32 id, len;
            txq.used_elem(txq.used_seen++, &id, &len);
            if (id < MAX_QUEUE)
                tx_busy[id] = false;
        }
    }

    void poll_rx() {
        bool refilled = false;
        while (rxq.used_seen != rxq.used_idx()) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            u32 id, len;
            rxq.used_elem(rxq.used_seen++, &id, &len);
            if (id >= rxq.size)
                continue;
            if (len > HDR)
                net::receive(this, rxq.bufs[id] + HDR, len - HDR);
            rxq.offer(static_cast<u16>(id));
            refilled = true;
        }
        if (refilled)
            rxq.kick();
    }
};

Nic* g_nics[8];
u32 g_nic_count = 0;

void irq(interrupts::Frame*) {
    for (u32 i = 0; i < g_nic_count; i++) {
        (void)*g_nics[i]->isr; // reading acknowledges
        g_nics[i]->irq_signal.signal();
    }
}

void rx_thread(void* arg) {
    auto* n = static_cast<Nic*>(arg);
    for (;;) {
        n->irq_signal.wait_timeout(5'000'000);
        n->poll_rx();
        LockGuard guard(n->tx_lock);
        n->reclaim_tx();
    }
}

bool match(const dev::Device* d) {
    return d->bus == dev::Bus::Pci && d->vendor == 0x1AF4 && (d->device == 0x1000 || d->device == 0x1041);
}

long probe(dev::Device* d) {
    pci::Function* f = pci::function_of(d);
    pci::enable(*f);
    auto* n = new Nic;
    // Walk the vendor capabilities to find the modern interface's regions.
    for (u8 cap = pci::read8(*f, 0x34) & 0xFC; cap; cap = pci::read8(*f, cap + 1) & 0xFC) {
        if (pci::read8(*f, cap) != 0x09)
            continue;
        u8 type = pci::read8(*f, cap + 3);
        u8 bar = pci::read8(*f, cap + 4);
        u32 offset = pci::read32(*f, cap + 8);
        auto* base = static_cast<volatile u8*>(pci::map_bar(*f, bar));
        if (!base)
            continue;
        switch (type) {
        case 1: n->common = base + offset; break;
        case 2: n->notify_base = base + offset; n->notify_mult = pci::read32(*f, cap + 16); break;
        case 3: n->isr = base + offset; break;
        case 4: n->device_cfg = base + offset; break;
        default: break;
        }
    }
    if (!n->common || !n->notify_base || !n->isr || !n->device_cfg) {
        klog::warn("virtio-net: no modern (virtio 1.0) interface");
        return -err::NODEV;
    }
    n->c_wr<u8>(20, 0); // reset
    while (n->c_rd<u8>(20))
        ;
    n->c_wr<u8>(20, STATUS_ACK | STATUS_DRIVER);
    n->c_wr<u32>(0, 0);
    u32 lo = n->c_rd<u32>(4);
    n->c_wr<u32>(0, 1);
    u32 hi = n->c_rd<u32>(4);
    if (!(hi & 1))
        return -err::NODEV; // not VERSION_1
    u32 want_lo = lo & ((1u << F_MAC) | (1u << F_STATUS));
    n->c_wr<u32>(8, 0);
    n->c_wr<u32>(12, want_lo);
    n->c_wr<u32>(8, 1);
    n->c_wr<u32>(12, 1u << (F_VERSION_1 - 32));
    n->c_wr<u8>(20, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK);
    if (!(n->c_rd<u8>(20) & STATUS_FEATURES_OK))
        return -err::NODEV;
    if (!n->setup_queue(n->rxq, 0, true) || !n->setup_queue(n->txq, 1, false))
        return -err::NOMEM;
    if (want_lo & (1u << F_MAC))
        for (int i = 0; i < 6; i++)
            n->mac[i] = n->device_cfg[i];
    if (want_lo & (1u << F_STATUS))
        n->link_up = *reinterpret_cast<volatile u16*>(n->device_cfg + 6) & 1;

    u8 vector = interrupts::allocate_vector();
    if (g_nic_count < 8 && pci::enable_msi(*f, vector)) {
        g_nics[g_nic_count++] = n;
        interrupts::set_handler(vector, irq);
    }
    n->c_wr<u8>(20, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    for (u16 i = 0; i < n->rxq.size; i++)
        n->rxq.offer(i);
    n->rxq.kick();

    n->hw = d;
    snprintf(d->name, sizeof(d->name), "Virtio network device");
    net::register_interface(n);
    sched::spawn("virtio-net-rx", rx_thread, n, -1, true);
    return 0;
}

dev::Driver g_driver{"virtio_net", match, probe, {}};

} // namespace

void register_driver() { dev::register_driver(&g_driver); }

} // namespace virtio_net
