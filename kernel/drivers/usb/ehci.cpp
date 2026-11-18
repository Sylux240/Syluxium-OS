// EHCI: USB 2.0 host controllers (PCs from about 2002 to 2015).
//
// The controller runs two schedules in memory: the asynchronous one (a ring
// of queue heads for control and bulk endpoints, serviced as bandwidth
// allows) and the periodic one (a 1024-entry frame list pointing at queue
// heads for interrupt endpoints, serviced every frame). Each queue head
// holds a chain of transfer descriptors (qTDs). Completion is found by
// polling the descriptors from a kernel thread every millisecond, which
// keeps working on chipsets whose interrupt routing we can't set up yet.
//
// Low/full-speed devices behind a high-speed hub use split transactions
// (the hub's transaction translator); that is how keyboards and mice reach
// EHCI on Intel chipsets with a "rate matching hub" (2011+). Such devices
// plugged straight into a root port belong to a USB 1.1 companion
// controller (UHCI/OHCI), which isn't driven yet.

#include <core/log.hpp>
#include <dev/device.hpp>
#include <drivers/pci/pci.hpp>
#include <drivers/usb/usb.hpp>
#include <lib/errno.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/layout.hpp>
#include <mm/pmm.hpp>
#include <sched/sched.hpp>
#include <sched/sync.hpp>
#include <time/clock.hpp>

namespace ehci {

namespace {

// Operational registers (offsets from CAPLENGTH).
constexpr u32 USBCMD = 0x00, USBSTS = 0x04, USBINTR = 0x08, CTRLDSSEGMENT = 0x10, PERIODICLISTBASE = 0x14,
              ASYNCLISTADDR = 0x18, CONFIGFLAG = 0x40, PORTSC = 0x44;
constexpr u32 CMD_RUN = 1, CMD_RESET = 2, CMD_PERIODIC = 1u << 4, CMD_ASYNC = 1u << 5, CMD_IAAD = 1u << 6;
constexpr u32 STS_HALTED = 1u << 12, STS_IAA = 1u << 5;
constexpr u32 PORT_CCS = 1, PORT_CSC = 2, PORT_PE = 4, PORT_PEC = 8, PORT_RESET = 1u << 8, PORT_POWER = 1u << 12,
              PORT_OWNER = 1u << 13, PORT_LINE_K = 1u << 10;
constexpr u32 PORT_RW1C = PORT_CSC | PORT_PEC | (1u << 5);

// Link pointer bits.
constexpr u32 LINK_TERMINATE = 1, TYPE_QH = 2;
// qTD token.
constexpr u32 TOKEN_ACTIVE = 1u << 7, TOKEN_HALTED = 1u << 6, TOKEN_IOC = 1u << 15;
constexpr u32 PID_OUT = 0, PID_IN = 1, PID_SETUP = 2;

struct alignas(32) Qtd {
    u32 next, alt_next, token;
    u32 buffer[5];
    u32 buffer_hi[5]; // 64-bit controllers
    u32 pad[3];
};
static_assert(sizeof(Qtd) == 64);

struct alignas(64) Qh {
    u32 link;
    u32 characteristics;
    u32 capabilities;
    u32 current;
    // Overlay (the qTD being worked on).
    u32 next, alt_next, token;
    u32 buffer[5];
    u32 buffer_hi[5];
    u32 pad[3];
};
static_assert(sizeof(Qh) == 96 || sizeof(Qh) == 128);

// A page below 4 GiB (the descriptors' and buffers' addresses are 32-bit).
u64 low_page(unsigned order = 0) {
    u64 rejected[32];
    int n = 0;
    u64 p = 0;
    for (int tries = 0; tries < 32; tries++) {
        p = pmm::alloc_pages(order, true);
        if (!p || p + (4096ull << order) <= 0x1'0000'0000ull)
            break;
        rejected[n++] = p;
        p = 0;
    }
    while (n > 0)
        pmm::free_pages(rejected[--n], order);
    return p;
}

class Controller;

class EhciDevice : public usb::Device {
public:
    EhciDevice(Controller* hc) : m_hc(hc) {}
    Controller* m_hc;
    u8 address = 0;
    u8 ep0_max = 8;
    u8 tt_hub = 0, tt_port = 0; // split transactions: high-speed hub address and its port
    bool hub_mtt = false;

    // One queue head per endpoint (index: number + 16 for IN), each with a
    // bounce buffer and a pool of qTDs.
    struct Pipe {
        Qh* qh = nullptr;
        u64 qh_phys = 0;
        Qtd* tds = nullptr;   // 16 qTDs in one page
        u64 tds_phys = 0;
        u8* buffer = nullptr;
        u64 buffer_phys = 0;
        u32 buffer_size = 0;
        unsigned buffer_order = 0;
        u16 max_packet = 8;
        bool periodic = false;
        bool toggle = false;
        u32 requested = 0;
        usb::InterruptCallback callback = nullptr;
        void* callback_ctx = nullptr;
    };
    Pipe pipes[32];

    long control(u8 type, u8 request, u16 value, u16 index, void* data, u16 length) override;
    long open_endpoint(const usb::Endpoint& ep) override;
    long transfer(u8 ep_address, void* data, u32 length) override { return bulk(ep_address, data, length, 5000); }
    long transfer_wait(u8 ep_address, void* data, u32 length) override { return bulk(ep_address, data, length, 1ull << 40); }
    long start_interrupt_in(u8 ep_address, u32 length, usb::InterruptCallback cb, void* ctx) override;
    long set_hub(u8 ports, bool multi_tt, u8) override {
        (void)ports;
        hub_mtt = multi_tt;
        return 0;
    }
    usb::Device* add_child(u8 hub_port, u8 child_speed) override;
    void disable() override;

    bool setup(); // address the device and read its EP0 packet size
    long bulk(u8 ep_address, void* data, u32 length, u64 timeout_ms);
    bool make_pipe(u8 index, u8 ep_num, bool in, u16 max_packet, bool periodic, u32 buffer_size);
    u32 characteristics(u8 ep_num, u16 max_packet, bool control_ep) const;
    u32 capabilities(bool periodic) const;
    void poll_interrupts();
};

class Controller {
public:
    dev::Device* entry = nullptr;
    volatile u8* cap = nullptr;
    volatile u8* op = nullptr;
    u32 ports = 0;
    u32 bus_number = 0;
    u32* frame_list = nullptr;
    Qh* async_head = nullptr;
    u64 async_head_phys = 0;
    Qh* periodic_head = nullptr;  // every interrupt QH hangs off this one
    u64 periodic_head_phys = 0;
    sched::Mutex lock;            // schedule changes, address allocation
    bool used_addresses[128] = {};
    EhciDevice* devices[128] = {};
    EhciDevice* root[16] = {};
    Controller* next = nullptr;

    u32 reg(u32 off) { return *reinterpret_cast<volatile u32*>(op + off); }
    void set(u32 off, u32 v) { *reinterpret_cast<volatile u32*>(op + off) = v; }

    bool init();
    void scan_ports();
    void enumerate_port(u32 port);
    u8 new_address() {
        LockGuard guard(lock);
        for (u8 a = 1; a < 128; a++)
            if (!used_addresses[a]) {
                used_addresses[a] = true;
                return a;
            }
        return 0;
    }
    // Async list: insert right after the head; remove with the doorbell.
    void async_add(Qh* qh, u64 phys) {
        LockGuard guard(lock);
        qh->link = async_head->link;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        async_head->link = static_cast<u32>(phys) | TYPE_QH;
    }
    void async_remove(u64 phys) {
        LockGuard guard(lock);
        Qh* prev = async_head;
        for (int guard_n = 0; guard_n < 512; guard_n++) {
            u32 nl = prev->link & ~0x1Fu;
            if (nl == static_cast<u32>(phys)) {
                prev->link = mm::phys_to_virt<Qh>(phys)->link;
                break;
            }
            if (nl == static_cast<u32>(async_head_phys))
                return;
            prev = mm::phys_to_virt<Qh>(nl);
        }
        // Interrupt-on-async-advance: the controller no longer caches it.
        set(USBSTS, STS_IAA);
        set(USBCMD, reg(USBCMD) | CMD_IAAD);
        u64 deadline = clock::now_ns() + 50'000'000;
        while (!(reg(USBSTS) & STS_IAA) && clock::now_ns() < deadline) {}
        set(USBSTS, STS_IAA);
    }
    void periodic_add(Qh* qh, u64 phys) {
        LockGuard guard(lock);
        qh->link = periodic_head->link;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        periodic_head->link = static_cast<u32>(phys) | TYPE_QH;
    }
    void periodic_remove(u64 phys) {
        LockGuard guard(lock);
        Qh* prev = periodic_head;
        while (!(prev->link & LINK_TERMINATE)) {
            u32 nl = prev->link & ~0x1Fu;
            if (nl == static_cast<u32>(phys)) {
                prev->link = mm::phys_to_virt<Qh>(phys)->link;
                break;
            }
            prev = mm::phys_to_virt<Qh>(nl);
        }
        sched::sleep_ms(2); // two frames: the controller is past it
    }
};

Controller* g_controllers = nullptr;

// ---- device ----

u32 EhciDevice::characteristics(u8 ep_num, u16 max_packet, bool control_ep) const {
    // Speeds (xHCI numbering in usb::Device::speed): 1 full, 2 low, 3 high.
    u32 eps = speed == 3 ? 2 : speed == 2 ? 1 : 0;
    u32 c = address | (static_cast<u32>(ep_num) << 8) | (eps << 12) | (static_cast<u32>(max_packet) << 16);
    if (control_ep)
        c |= 1u << 14; // DTC: the data toggle comes from each qTD
    if (control_ep && speed != 3)
        c |= 1u << 27; // C: a full/low-speed control endpoint
    c |= 4u << 28;     // NAK reload
    return c;
}

u32 EhciDevice::capabilities(bool periodic) const {
    u32 c = 1u << 30; // one transaction per microframe
    if (speed != 3) // split transactions through the hub's TT
        c |= (static_cast<u32>(tt_hub) << 16) | (static_cast<u32>(tt_port) << 23);
    if (periodic)
        c |= speed == 3 ? 0x01 : (0x01 | (0x1Cu << 8)); // S-mask uframe 0; C-mask uframes 2-4 for splits
    return c;
}

bool EhciDevice::make_pipe(u8 index, u8 ep_num, bool in, u16 max_packet, bool periodic, u32 buffer_size) {
    Pipe& p = pipes[index];
    if (!p.qh) {
        p.qh_phys = low_page();
        p.tds_phys = low_page();
        if (!p.qh_phys || !p.tds_phys)
            return false;
        p.qh = mm::phys_to_virt<Qh>(p.qh_phys);
        p.tds = mm::phys_to_virt<Qtd>(p.tds_phys);
        unsigned order = 0;
        while ((4096u << order) < buffer_size)
            order++;
        p.buffer_phys = low_page(order);
        if (!p.buffer_phys)
            return false;
        p.buffer = mm::phys_to_virt<u8>(p.buffer_phys);
        p.buffer_size = 4096u << order;
        p.buffer_order = order;
    }
    (void)in;
    p.max_packet = max_packet;
    p.periodic = periodic;
    memset(p.qh, 0, sizeof(Qh));
    p.qh->characteristics = characteristics(ep_num, max_packet, index == 0);
    p.qh->capabilities = capabilities(periodic);
    p.qh->next = LINK_TERMINATE;
    p.qh->alt_next = LINK_TERMINATE;
    p.qh->current = 0;
    if (periodic)
        m_hc->periodic_add(p.qh, p.qh_phys);
    else
        m_hc->async_add(p.qh, p.qh_phys);
    return true;
}

// Fill qTD `i` of pipe p: `len` bytes at buffer offset `off`.
void fill_td(EhciDevice::Pipe& p, int i, u32 pid, u32 off, u32 len, bool toggle, bool ioc, bool last) {
    Qtd& t = p.tds[i];
    memset(&t, 0, sizeof(t));
    t.next = last ? LINK_TERMINATE : static_cast<u32>(p.tds_phys + static_cast<u64>(i + 1) * sizeof(Qtd));
    // A short IN packet ends the transfer: continue at an inactive dummy
    // (the last slot), which leaves the queue head idle.
    Qtd& dummy = p.tds[15];
    dummy.next = LINK_TERMINATE;
    dummy.alt_next = LINK_TERMINATE;
    dummy.token = 0;
    t.alt_next = (pid == PID_IN && !last) ? static_cast<u32>(p.tds_phys + 15 * sizeof(Qtd)) : LINK_TERMINATE;
    t.token = TOKEN_ACTIVE | (pid << 8) | (3u << 10) | (len << 16) | (toggle ? 1u << 31 : 0) | (ioc ? TOKEN_IOC : 0);
    u64 addr = p.buffer_phys + off;
    t.buffer[0] = static_cast<u32>(addr);
    u64 page = addr & ~0xFFFull;
    for (int k = 1; k < 5; k++)
        t.buffer[k] = static_cast<u32>(page + 4096ull * k);
}

// Run the chain starting at qTD 0 of pipe p; wait for the last one.
long run_chain(EhciDevice* d, EhciDevice::Pipe& p, int count, u64 timeout_ms, u32* residual_out) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    p.qh->token &= 1u << 31; // clear halt/errors, keep the data toggle
    p.qh->next = static_cast<u32>(p.tds_phys);
    u64 deadline = clock::now_ns() + timeout_ms * 1'000'000;
    for (;;) {
        if (d->gone)
            return -err::NODEV;
        bool active = false, failed = false;
        u32 residual = 0;
        for (int i = 0; i < count; i++) {
            u32 tok = __atomic_load_n(&p.tds[i].token, __ATOMIC_ACQUIRE);
            if (tok & TOKEN_ACTIVE) {
                active = true;
                break;
            }
            if (tok & TOKEN_HALTED)
                failed = true;
            residual += (tok >> 16) & 0x7FFF;
            // A short packet ends the transfer early (the rest stay inactive? no: we stop here).
            if (((tok >> 16) & 0x7FFF) && i < count - 1 && ((tok >> 8) & 3) == PID_IN) {
                // Remaining qTDs never run: deactivate them.
                for (int k = i + 1; k < count; k++)
                    p.tds[k].token &= ~TOKEN_ACTIVE;
                break;
            }
        }
        if (failed) {
            p.qh->next = LINK_TERMINATE;
            p.qh->token &= 1u << 31;
            return -err::IO;
        }
        if (!active) {
            if (residual_out)
                *residual_out = residual;
            return 0;
        }
        if (clock::now_ns() > deadline) {
            for (int i = 0; i < count; i++)
                p.tds[i].token &= ~TOKEN_ACTIVE;
            p.qh->next = LINK_TERMINATE;
            return -err::TIMEDOUT;
        }
        sched::sleep_ms(1);
    }
}

long EhciDevice::control(u8 type, u8 request, u16 value, u16 index, void* data, u16 length) {
    Pipe& p = pipes[0];
    if (gone)
        return -err::NODEV;
    if (!p.qh && !make_pipe(0, 0, false, ep0_max, false, 4096))
        return -err::NOMEM;
    if (length > p.buffer_size - 8)
        return -err::INVAL;
    // Setup packet at the end of the buffer, data at the start.
    u8* setup = p.buffer + p.buffer_size - 8;
    setup[0] = type;
    setup[1] = request;
    setup[2] = static_cast<u8>(value);
    setup[3] = static_cast<u8>(value >> 8);
    setup[4] = static_cast<u8>(index);
    setup[5] = static_cast<u8>(index >> 8);
    setup[6] = static_cast<u8>(length);
    setup[7] = static_cast<u8>(length >> 8);
    bool in = type & 0x80;
    if (!in && length)
        memcpy(p.buffer, data, length);
    p.qh->characteristics = characteristics(0, ep0_max, true);
    int n = 0;
    fill_td(p, n++, PID_SETUP, p.buffer_size - 8, 8, false, false, false);
    if (length)
        fill_td(p, n++, in ? PID_IN : PID_OUT, 0, length, true, false, false);
    fill_td(p, n++, (in && length) ? PID_OUT : PID_IN, 0, 0, true, true, true);
    u32 residual = 0;
    if (long r = run_chain(this, p, n, 2000, &residual); r < 0)
        return r;
    u32 actual = length ? length - ((p.tds[1].token >> 16) & 0x7FFF) : 0;
    if (in && actual)
        memcpy(data, p.buffer, actual);
    return actual;
}

long EhciDevice::open_endpoint(const usb::Endpoint& e) {
    u8 index = static_cast<u8>((e.address & 0xF) + (e.is_in() ? 16 : 0));
    if (pipes[index].qh)
        return 0;
    bool periodic = e.type() == 3;
    return make_pipe(index, e.address & 0xF, e.is_in(), e.max_packet ? e.max_packet : 64, periodic,
                     periodic ? 4096 : 64 * 1024) ? 0 : -err::NOMEM;
}

long EhciDevice::bulk(u8 ep_address, void* data, u32 length, u64 timeout_ms) {
    u8 index = static_cast<u8>((ep_address & 0xF) + ((ep_address & 0x80) ? 16 : 0));
    Pipe& p = pipes[index];
    if (gone)
        return -err::NODEV;
    if (!p.qh || p.periodic || length > p.buffer_size)
        return -err::INVAL;
    bool in = ep_address & 0x80;
    if (!in)
        memcpy(p.buffer, data, length);
    // One qTD moves up to 16 KiB here (page-aligned buffer: 4 full pages).
    int n = 0;
    u32 off = 0;
    do {
        u32 chunk = min<u32>(length - off, 16 * 1024);
        bool last = off + chunk >= length;
        fill_td(p, n, in ? PID_IN : PID_OUT, off, chunk, p.toggle, last, last);
        // Toggles for the next chunk: each packet flips it.
        u32 packets = chunk ? (chunk + p.max_packet - 1) / p.max_packet : 1;
        if (packets & 1)
            p.toggle = !p.toggle;
        off += chunk;
        n++;
    } while (off < length && n < 15);
    // The controller keeps the toggle in the overlay when DTC is clear; we
    // drive it from the qTDs for control only, so let the QH track it.
    p.qh->characteristics = characteristics(ep_address & 0xF, p.max_packet, false);
    u32 residual = 0;
    long r = run_chain(this, p, n, timeout_ms, &residual);
    if (r < 0)
        return r;
    // The QH overlay now holds the right toggle for the next transfer.
    p.toggle = (p.qh->token >> 31) & 1;
    u32 actual = length - residual;
    if (in && actual)
        memcpy(data, p.buffer, actual);
    return actual;
}

long EhciDevice::start_interrupt_in(u8 ep_address, u32 length, usb::InterruptCallback cb, void* ctx) {
    u8 index = static_cast<u8>((ep_address & 0xF) + 16);
    Pipe& p = pipes[index];
    if (!p.qh || !p.periodic)
        return -err::INVAL;
    p.requested = min<u32>(length, p.buffer_size);
    p.callback_ctx = ctx;
    p.callback = cb;
    fill_td(p, 0, PID_IN, 0, p.requested, false, true, true);
    p.qh->token = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    p.qh->next = static_cast<u32>(p.tds_phys);
    return 0;
}

// From the poll thread: completed interrupt qTDs -> callbacks, then re-arm.
void EhciDevice::poll_interrupts() {
    for (int index = 16; index < 32; index++) {
        Pipe& p = pipes[index];
        if (!p.callback || gone)
            continue;
        u32 tok = __atomic_load_n(&p.tds[0].token, __ATOMIC_ACQUIRE);
        if (tok & TOKEN_ACTIVE)
            continue;
        if (!(tok & TOKEN_HALTED)) {
            u32 got = p.requested - ((tok >> 16) & 0x7FFF);
            if (got)
                p.callback(p.callback_ctx, p.buffer, got);
        }
        fill_td(p, 0, PID_IN, 0, p.requested, false, true, true);
        p.qh->token &= 1u << 31; // keep the toggle, clear halt/errors
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        p.qh->next = static_cast<u32>(p.tds_phys);
    }
}

bool EhciDevice::setup() {
    // EP0 packet size: 64 for high speed; full/low speed: read the first 8
    // bytes of the device descriptor at 8 first.
    ep0_max = speed == 3 ? 64 : 8;
    u8 desc[8];
    if (speed != 3 && control(0x80, usb::REQ_GET_DESCRIPTOR, usb::DESC_DEVICE << 8, 0, desc, 8) == 8 && desc[7])
        ep0_max = desc[7];
    u8 a = m_hc->new_address();
    if (!a)
        return false;
    if (control(0x00, 5 /* SET_ADDRESS */, a, 0, nullptr, 0) < 0)
        return false;
    sched::sleep_ms(2); // USB 2.0 9.2.6.3: 2 ms to switch addresses
    address = a;
    m_hc->devices[a] = this;
    if (speed == 3 && control(0x80, usb::REQ_GET_DESCRIPTOR, usb::DESC_DEVICE << 8, 0, desc, 8) == 8 && desc[7])
        ep0_max = desc[7];
    return true;
}

usb::Device* EhciDevice::add_child(u8 hub_port, u8 child_speed) {
    auto* d = new EhciDevice(m_hc);
    d->speed = child_speed;
    d->controller = controller;
    d->hub_depth = static_cast<u8>(hub_depth + 1);
    snprintf(d->path, sizeof(d->path), "%s.%u", path, hub_port);
    if (child_speed == 1 || child_speed == 2) { // the nearest high-speed hub translates
        if (speed == 3) {
            d->tt_hub = address;
            d->tt_port = hub_port;
        } else {
            d->tt_hub = tt_hub;
            d->tt_port = tt_port;
        }
    }
    if (!d->setup()) {
        d->disable();
        return nullptr;
    }
    return d;
}

void EhciDevice::disable() {
    if (gone)
        return;
    gone = true;
    for (int i = 31; i >= 0; i--) {
        Pipe& p = pipes[i];
        if (!p.qh)
            continue;
        p.callback = nullptr;
        if (p.periodic)
            m_hc->periodic_remove(p.qh_phys);
        else
            m_hc->async_remove(p.qh_phys);
    }
    sched::sleep_ms(5); // poll thread and waiters see `gone`
    for (Pipe& p : pipes) {
        if (!p.qh)
            continue;
        pmm::free_pages(p.qh_phys, 0);
        pmm::free_pages(p.tds_phys, 0);
        pmm::free_pages(p.buffer_phys, p.buffer_order);
        p = Pipe{};
    }
    if (address) {
        LockGuard guard(m_hc->lock);
        m_hc->used_addresses[address] = false;
        m_hc->devices[address] = nullptr;
    }
}

// ---- controller ----

bool Controller::init() {
    u8 caplen = cap[0];
    op = cap + caplen;
    u32 hcs = *reinterpret_cast<volatile u32*>(cap + 4);
    ports = hcs & 0xF;
    // Stop and reset.
    set(USBCMD, reg(USBCMD) & ~CMD_RUN);
    u64 deadline = clock::now_ns() + 20'000'000;
    while (!(reg(USBSTS) & STS_HALTED) && clock::now_ns() < deadline) {}
    set(USBCMD, CMD_RESET);
    deadline = clock::now_ns() + 250'000'000;
    while ((reg(USBCMD) & CMD_RESET) && clock::now_ns() < deadline) {}
    if (reg(USBCMD) & CMD_RESET)
        return false;
    set(CTRLDSSEGMENT, 0);
    set(USBINTR, 0); // completion is polled
    // Async schedule: an empty head queue head pointing at itself.
    async_head_phys = low_page();
    u64 periodic_phys = low_page();
    u64 frames_phys = low_page();
    if (!async_head_phys || !periodic_phys || !frames_phys)
        return false;
    async_head = mm::phys_to_virt<Qh>(async_head_phys);
    async_head->link = static_cast<u32>(async_head_phys) | TYPE_QH;
    async_head->characteristics = 1u << 15; // H: head of the reclamation list
    async_head->next = LINK_TERMINATE;
    async_head->alt_next = LINK_TERMINATE;
    async_head->token = TOKEN_HALTED;
    // Periodic schedule: every frame points at one (inactive) head QH;
    // interrupt QHs hang off it, so they're polled every frame (1 ms).
    periodic_head = mm::phys_to_virt<Qh>(periodic_phys);
    periodic_head_phys = periodic_phys;
    periodic_head->link = LINK_TERMINATE;
    periodic_head->next = LINK_TERMINATE;
    periodic_head->alt_next = LINK_TERMINATE;
    periodic_head->token = TOKEN_HALTED;
    frame_list = mm::phys_to_virt<u32>(frames_phys);
    for (int i = 0; i < 1024; i++)
        frame_list[i] = static_cast<u32>(periodic_phys) | TYPE_QH;
    set(PERIODICLISTBASE, static_cast<u32>(frames_phys));
    set(ASYNCLISTADDR, static_cast<u32>(async_head_phys));
    set(USBCMD, CMD_RUN | CMD_ASYNC | CMD_PERIODIC | (8u << 16)); // interrupt threshold 1 ms
    set(CONFIGFLAG, 1); // every port to EHCI (not the companions)
    deadline = clock::now_ns() + 20'000'000;
    while ((reg(USBSTS) & STS_HALTED) && clock::now_ns() < deadline) {}
    // Port power (if the controller switches it).
    if (hcs & (1u << 4))
        for (u32 i = 0; i < ports; i++)
            set(PORTSC + 4 * i, reg(PORTSC + 4 * i) | PORT_POWER);
    sched::sleep_ms(20);
    return !(reg(USBSTS) & STS_HALTED);
}

void Controller::enumerate_port(u32 port) {
    u32 off = PORTSC + 4 * (port - 1);
    u32 sc = reg(off);
    set(off, (sc & ~PORT_RW1C) | (sc & PORT_RW1C)); // clear change bits
    if (!(sc & PORT_CCS))
        return;
    if ((sc & (3u << 10)) == PORT_LINE_K) { // K-state: a low-speed device
        set(off, reg(off) | PORT_OWNER);    // the companion controller's (not driven yet)
        klog::warn("ehci: low-speed device on port %u needs a USB 1.1 controller (not supported yet)", port);
        return;
    }
    // Reset: 50 ms, then clear; enabled afterwards = high speed.
    set(off, (reg(off) & ~(PORT_PE | PORT_RW1C)) | PORT_RESET);
    sched::sleep_ms(50);
    set(off, reg(off) & ~(PORT_RESET | PORT_RW1C));
    u64 deadline = clock::now_ns() + 20'000'000;
    while ((reg(off) & PORT_RESET) && clock::now_ns() < deadline) {}
    sched::sleep_ms(10);
    if (!(reg(off) & PORT_PE)) {
        set(off, reg(off) | PORT_OWNER);
        klog::warn("ehci: full-speed device on port %u needs a USB 1.1 controller (not supported yet)", port);
        return;
    }
    auto* d = new EhciDevice(this);
    d->speed = 3;
    d->controller = entry;
    snprintf(d->path, sizeof(d->path), "%u-%u", bus_number, port);
    if (!d->setup()) {
        klog::warn("ehci: device on port %u could not be addressed", port);
        d->disable();
        return;
    }
    if (port < 16)
        root[port] = d;
    usb::attach(d);
}

void Controller::scan_ports() {
    for (u32 p = 1; p <= ports; p++)
        enumerate_port(p);
}

void poll_thread(void* arg) {
    auto* hc = static_cast<Controller*>(arg);
    u32 tick = 0;
    for (;;) {
        sched::sleep_ms(1);
        for (EhciDevice* d : hc->devices)
            if (d && !d->gone)
                d->poll_interrupts();
        // Root port changes (hot-plug), every 250 ms, on the USB worker.
        if (++tick % 250 == 0) {
            for (u32 p = 1; p <= hc->ports; p++) {
                u32 sc = hc->reg(PORTSC + 4 * (p - 1));
                if (!(sc & PORT_CSC) || (sc & PORT_OWNER))
                    continue;
                struct Work { Controller* hc; u32 port; };
                auto* w = new Work{hc, p};
                usb::queue_work([](void* a) {
                    auto* work = static_cast<Work*>(a);
                    Controller* c = work->hc;
                    u32 port = work->port;
                    delete work;
                    if (port < 16 && c->root[port]) {
                        usb::detach(c->root[port]);
                        c->root[port] = nullptr;
                    }
                    sched::sleep_ms(100); // debounce
                    c->enumerate_port(port);
                }, w);
            }
        }
    }
}

bool match(const dev::Device* d) { return d->bus == dev::Bus::Pci && d->class_code == 0x0C0320; }

long probe(dev::Device* d) {
    pci::Function* f = pci::function_of(d);
    pci::enable(*f);
    auto* cap = static_cast<volatile u8*>(pci::map_bar(*f, 0));
    if (!cap)
        return -err::IO;
    // Take it from the firmware (USB legacy support capability): its PS/2
    // emulation stops, and the controller is ours.
    u32 hccparams = *reinterpret_cast<volatile u32*>(cap + 0x08);
    u8 eecp = static_cast<u8>(hccparams >> 8);
    for (int guard = 0; eecp >= 0x40 && guard < 16; guard++) {
        u32 v = pci::read32(*f, eecp);
        if ((v & 0xFF) == 1) { // USBLEGSUP
            pci::write32(*f, eecp, v | (1u << 24));
            u64 deadline = clock::now_ns() + 1'000'000'000;
            while ((pci::read32(*f, eecp) & (1u << 16)) && clock::now_ns() < deadline) {}
            if (pci::read32(*f, eecp) & (1u << 16))
                pci::write32(*f, eecp, (pci::read32(*f, eecp) & ~(1u << 16)) | (1u << 24));
            pci::write32(*f, eecp + 4, 0); // USBLEGCTLSTS: no SMIs
        }
        eecp = static_cast<u8>(v >> 8);
    }
    auto* hc = new Controller;
    hc->entry = d;
    hc->cap = cap;
    if (!hc->init()) {
        snprintf(d->status_text, sizeof(d->status_text), "the controller did not start");
        return -err::IO;
    }
    u32 n = 1;
    for (Controller* c = g_controllers; c; c = c->next)
        n++;
    hc->bus_number = 100 + n; // after xHCI buses in paths: "101-2"
    hc->next = g_controllers;
    g_controllers = hc;
    snprintf(d->name, sizeof(d->name), "USB 2 host controller (EHCI, %u ports)", hc->ports);
    sched::spawn("ehci-poll", poll_thread, hc, -1, true);
    return 0;
}

dev::Driver g_driver{"ehci_hcd", match, probe, {}};

} // namespace

void register_driver() { dev::register_driver(&g_driver); }

void enumerate_all() {
    for (Controller* c = g_controllers; c; c = c->next)
        c->scan_ports();
}

} // namespace ehci
