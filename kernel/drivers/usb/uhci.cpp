// UHCI: Intel's USB 1.1 host controller (and the companion controllers next
// to EHCI on Intel chipsets until about 2011). That's where USB keyboards
// and mice end up on those PCs: they're low/full-speed devices, which EHCI
// hands to its companions.
//
// One transfer descriptor (TD) per packet; queue heads (QHs) per endpoint
// hang off a skeleton: every frame -> interrupt head -> interrupt QHs ->
// async head -> control/bulk QHs. Completion is polled every millisecond.

#include <arch/x86_64/cpu.hpp>
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

namespace uhci {

namespace {

constexpr u16 USBCMD = 0x00, USBSTS = 0x02, USBINTR = 0x04, FRNUM = 0x06, FRBASEADD = 0x08, SOFMOD = 0x0C,
              PORTSC1 = 0x10;
constexpr u16 CMD_RUN = 1, CMD_HCRESET = 2, CMD_GRESET = 4, CMD_MAXP64 = 0x80;
constexpr u16 STS_HALTED = 0x20;
constexpr u16 PORT_CCS = 1, PORT_CSC = 2, PORT_PE = 4, PORT_PEC = 8, PORT_LSDA = 0x100, PORT_RESET = 0x200;
constexpr u32 LINK_T = 1, LINK_QH = 2, LINK_VF = 4;
constexpr u32 TD_ACTIVE = 1u << 23, TD_STALLED = 1u << 22, TD_IOC = 1u << 24, TD_LS = 1u << 26, TD_SPD = 1u << 29;
constexpr u8 PID_SETUP = 0x2D, PID_IN = 0x69, PID_OUT = 0xE1;

struct alignas(16) Td {
    u32 link, status, token, buffer;
    u32 pad[4];
};
static_assert(sizeof(Td) == 32);
struct alignas(16) Qh {
    u32 head, element;
    u32 pad[2];
};

u64 low_pages(unsigned order) {
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

class UhciDevice : public usb::Device {
public:
    explicit UhciDevice(Controller* hc) : m_hc(hc) {}
    Controller* m_hc;
    u8 address = 0;
    u8 ep0_max = 8;

    struct Pipe {
        Qh* qh = nullptr;
        u64 qh_phys = 0;
        Td* tds = nullptr;        // TD pool (pages)
        u64 tds_phys = 0;
        u32 td_count = 0;
        unsigned tds_order = 0;
        u8* buffer = nullptr;
        u64 buffer_phys = 0;
        u32 buffer_size = 0;
        unsigned buffer_order = 0;
        u16 max_packet = 8;
        u8 ep_num = 0;
        bool periodic = false;
        bool toggle = false;
        u32 requested = 0;
        u32 queued = 0;           // interrupt pipe: TDs in flight
        usb::InterruptCallback callback = nullptr;
        void* callback_ctx = nullptr;
    };
    Pipe pipes[32];

    long control(u8 type, u8 request, u16 value, u16 index, void* data, u16 length) override;
    long open_endpoint(const usb::Endpoint& ep) override;
    long transfer(u8 ep_address, void* data, u32 length) override { return bulk(ep_address, data, length, 5000); }
    long transfer_wait(u8 ep_address, void* data, u32 length) override { return bulk(ep_address, data, length, 1ull << 40); }
    long start_interrupt_in(u8 ep_address, u32 length, usb::InterruptCallback cb, void* ctx) override;
    long set_hub(u8, bool, u8) override { return 0; }
    usb::Device* add_child(u8 hub_port, u8 child_speed) override;
    void disable() override;

    bool setup();
    bool make_pipe(u8 index, u8 ep_num, u16 max_packet, bool periodic, u32 buffer_size, u32 tds);
    long bulk(u8 ep_address, void* data, u32 length, u64 timeout_ms);
    void poll_interrupts();
    u32 td_status(bool ioc) const { return TD_ACTIVE | (3u << 27) | (speed == 2 ? TD_LS : 0) | (ioc ? TD_IOC : 0); }
    u32 td_token(u8 pid, u8 ep, bool toggle, u32 len) const {
        u32 maxlen = len ? len - 1 : 0x7FF;
        return pid | (static_cast<u32>(address) << 8) | (static_cast<u32>(ep) << 15) | (toggle ? 1u << 19 : 0) |
               (maxlen << 21);
    }
};

class Controller {
public:
    dev::Device* entry = nullptr;
    u16 io = 0;
    u32 bus_number = 0;
    u32* frames = nullptr;
    Qh* int_head = nullptr;
    u64 int_head_phys = 0;
    Qh* async_head = nullptr;
    u64 async_head_phys = 0;
    sched::Mutex lock;
    bool used[128] = {};
    UhciDevice* devices[128] = {};
    UhciDevice* root[2] = {};
    Controller* next = nullptr;

    u16 r16(u16 o) { return arch::inw(static_cast<u16>(io + o)); }
    void w16(u16 o, u16 v) { arch::outw(static_cast<u16>(io + o), v); }
    void w32(u16 o, u32 v) { asm volatile("outl %0, %1" : : "a"(v), "Nd"(static_cast<u16>(io + o))); }

    bool init();
    void enumerate_port(u32 port);
    u8 new_address() {
        LockGuard guard(lock);
        for (u8 a = 1; a < 128; a++)
            if (!used[a]) {
                used[a] = true;
                return a;
            }
        return 0;
    }
    void insert(Qh* head, Qh* qh, u64 phys) {
        LockGuard guard(lock);
        qh->head = head->head;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        head->head = static_cast<u32>(phys) | LINK_QH;
    }
    void remove(Qh* head, u64 phys) {
        LockGuard guard(lock);
        Qh* prev = head;
        for (int i = 0; i < 512 && !(prev->head & LINK_T); i++) {
            u32 nl = prev->head & ~0xFu;
            if (nl == static_cast<u32>(phys)) {
                prev->head = mm::phys_to_virt<Qh>(phys)->head;
                break;
            }
            if (nl == static_cast<u32>(async_head_phys) && head != async_head)
                break; // end of the interrupt part
            prev = mm::phys_to_virt<Qh>(nl);
        }
        sched::sleep_ms(2);
    }
};

Controller* g_controllers = nullptr;

bool UhciDevice::make_pipe(u8 index, u8 ep_num, u16 max_packet, bool periodic, u32 buffer_size, u32 tds) {
    Pipe& p = pipes[index];
    if (!p.qh) {
        p.qh_phys = low_pages(0);
        unsigned order = 0;
        while ((4096u << order) < tds * sizeof(Td))
            order++;
        p.tds_phys = low_pages(order);
        p.tds_order = order;
        unsigned border = 0;
        while ((4096u << border) < buffer_size)
            border++;
        p.buffer_phys = low_pages(border);
        if (!p.qh_phys || !p.tds_phys || !p.buffer_phys)
            return false;
        p.qh = mm::phys_to_virt<Qh>(p.qh_phys);
        p.tds = mm::phys_to_virt<Td>(p.tds_phys);
        p.td_count = (4096u << order) / sizeof(Td);
        p.buffer = mm::phys_to_virt<u8>(p.buffer_phys);
        p.buffer_size = 4096u << border;
        p.buffer_order = border;
    }
    p.max_packet = max_packet;
    p.ep_num = ep_num;
    p.periodic = periodic;
    p.qh->element = LINK_T;
    m_hc->insert(periodic ? m_hc->int_head : m_hc->async_head, p.qh, p.qh_phys);
    return true;
}

// Build a TD chain for `len` bytes at buffer offset `off` (one TD per packet).
// Returns the number of TDs, or 0 if the pool is too small.
u32 build(UhciDevice* d, UhciDevice::Pipe& p, u32 first, u8 pid, u32 off, u32 len, bool* toggle, bool ioc_last) {
    u32 count = len ? (len + p.max_packet - 1) / p.max_packet : 1;
    if (first + count > p.td_count)
        return 0;
    for (u32 i = 0; i < count; i++) {
        Td& t = p.tds[first + i];
        u32 chunk = len ? min<u32>(p.max_packet, len - i * p.max_packet) : 0;
        bool last = i + 1 == count;
        t.link = last ? LINK_T : static_cast<u32>(p.tds_phys + (first + i + 1) * sizeof(Td)) | LINK_VF;
        t.status = d->td_status(last && ioc_last) | (pid == PID_IN ? TD_SPD : 0);
        t.token = d->td_token(pid, p.ep_num, *toggle, chunk);
        t.buffer = static_cast<u32>(p.buffer_phys + off + i * p.max_packet);
        *toggle = !*toggle;
    }
    return count;
}

// Run TDs [0, count) of pipe p; returns bytes moved (IN/OUT data TDs from
// `data_first` on) or -errno. Short packets end the transfer.
long run(UhciDevice* d, UhciDevice::Pipe& p, u32 count, u32 data_first, u32 data_count, u64 timeout_ms, bool* toggle_out) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    p.qh->element = static_cast<u32>(p.tds_phys);
    u64 deadline = clock::now_ns() + timeout_ms * 1'000'000;
    for (;;) {
        if (d->gone) {
            p.qh->element = LINK_T;
            return -err::NODEV;
        }
        u32 moved = 0;
        bool active = false, failed = false, shortp = false;
        u32 last_done = 0;
        for (u32 i = 0; i < count; i++) {
            u32 st = __atomic_load_n(&p.tds[i].status, __ATOMIC_ACQUIRE);
            if (st & TD_ACTIVE) {
                active = true;
                break;
            }
            if (st & TD_STALLED || (st & (0x3Eu << 17)) == (0x3Eu << 17)) {
                failed = true;
                break;
            }
            u32 actual = ((st & 0x7FF) + 1) & 0x7FF;
            u32 maxlen = ((p.tds[i].token >> 21) + 1) & 0x7FF;
            if (i >= data_first && i < data_first + data_count)
                moved += actual;
            last_done = i;
            if (actual < maxlen && i >= data_first && i < data_first + data_count) {
                shortp = true;
                break;
            }
        }
        if (failed) {
            p.qh->element = LINK_T;
            return -err::IO;
        }
        if (shortp && toggle_out) // next packet expects the toggle after the short one
            *toggle_out = !((p.tds[last_done].token >> 19) & 1);
        if (shortp || !active) {
            p.qh->element = LINK_T;
            return moved;
        }
        if (clock::now_ns() > deadline) {
            p.qh->element = LINK_T;
            return -err::TIMEDOUT;
        }
        sched::sleep_ms(1);
    }
}

long UhciDevice::control(u8 type, u8 request, u16 value, u16 index, void* data, u16 length) {
    Pipe& p = pipes[0];
    if (gone)
        return -err::NODEV;
    if (!p.qh && !make_pipe(0, 0, ep0_max, false, 4096, 128))
        return -err::NOMEM;
    p.max_packet = ep0_max;
    u8* setup = p.buffer + p.buffer_size - 8;
    u8 pkt[8] = {type, request, static_cast<u8>(value), static_cast<u8>(value >> 8), static_cast<u8>(index),
                 static_cast<u8>(index >> 8), static_cast<u8>(length), static_cast<u8>(length >> 8)};
    memcpy(setup, pkt, 8);
    bool in = type & 0x80;
    if (!in && length)
        memcpy(p.buffer, data, length);
    // Setup TD.
    Td& s = p.tds[0];
    s.status = td_status(false);
    s.token = td_token(PID_SETUP, 0, false, 8);
    s.buffer = static_cast<u32>(p.buffer_phys + p.buffer_size - 8);
    u32 n = 1;
    bool toggle = true;
    u32 data_tds = 0;
    if (length) {
        data_tds = build(this, p, 1, in ? PID_IN : PID_OUT, 0, length, &toggle, false);
        if (!data_tds)
            return -err::INVAL;
        n += data_tds;
    }
    s.link = static_cast<u32>(p.tds_phys + sizeof(Td)) | LINK_VF;
    p.tds[n - 1].link = static_cast<u32>(p.tds_phys + n * sizeof(Td)) | LINK_VF;
    // Status TD (opposite direction, toggle 1).
    Td& st = p.tds[n];
    st.link = LINK_T;
    st.status = td_status(true);
    st.token = td_token((in && length) ? PID_OUT : PID_IN, 0, true, 0);
    st.buffer = 0;
    long r = run(this, p, n + 1, 1, data_tds, 2000, nullptr);
    if (r < 0)
        return r;
    // A short IN data stage: the status stage still has to run.
    if (in && length && static_cast<u32>(r) < length && (p.tds[n].status & TD_ACTIVE)) {
        p.qh->element = static_cast<u32>(p.tds_phys + n * sizeof(Td));
        u64 deadline = clock::now_ns() + 500'000'000;
        while ((p.tds[n].status & TD_ACTIVE) && clock::now_ns() < deadline)
            sched::sleep_ms(1);
        p.qh->element = LINK_T;
    }
    if (in && r > 0)
        memcpy(data, p.buffer, static_cast<usize>(r));
    return r;
}

long UhciDevice::open_endpoint(const usb::Endpoint& e) {
    u8 index = static_cast<u8>((e.address & 0xF) + (e.is_in() ? 16 : 0));
    if (pipes[index].qh)
        return 0;
    bool periodic = e.type() == 3;
    // Bulk: 4 KiB per round (64 TDs of 64 bytes); the caller's larger
    // transfers are split in bulk().
    return make_pipe(index, e.address & 0xF, e.max_packet ? e.max_packet : 8, periodic, periodic ? 4096 : 16384,
                     periodic ? 128 : 512) ? 0 : -err::NOMEM;
}

long UhciDevice::bulk(u8 ep_address, void* data, u32 length, u64 timeout_ms) {
    u8 index = static_cast<u8>((ep_address & 0xF) + ((ep_address & 0x80) ? 16 : 0));
    Pipe& p = pipes[index];
    if (gone)
        return -err::NODEV;
    if (!p.qh || p.periodic)
        return -err::INVAL;
    bool in = ep_address & 0x80;
    u32 done = 0;
    u32 round_max = min<u32>(p.buffer_size, (p.td_count - 1) * p.max_packet);
    do {
        u32 chunk = min<u32>(length - done, round_max);
        if (!in)
            memcpy(p.buffer, static_cast<u8*>(data) + done, chunk);
        bool toggle = p.toggle;
        u32 n = build(this, p, 0, in ? PID_IN : PID_OUT, 0, chunk, &toggle, true);
        if (!n)
            return -err::INVAL;
        bool after_short = toggle;
        long r = run(this, p, n, 0, n, timeout_ms, &after_short);
        if (r < 0)
            return r;
        p.toggle = static_cast<u32>(r) < chunk ? after_short : toggle;
        if (in)
            memcpy(static_cast<u8*>(data) + done, p.buffer, static_cast<usize>(r));
        done += static_cast<u32>(r);
        if (static_cast<u32>(r) < chunk)
            break; // short packet: the device has no more
    } while (done < length);
    return done;
}

long UhciDevice::start_interrupt_in(u8 ep_address, u32 length, usb::InterruptCallback cb, void* ctx) {
    u8 index = static_cast<u8>((ep_address & 0xF) + 16);
    Pipe& p = pipes[index];
    if (!p.qh || !p.periodic)
        return -err::INVAL;
    p.requested = min<u32>(length, p.max_packet); // one packet per report
    p.callback_ctx = ctx;
    p.callback = cb;
    bool toggle = p.toggle;
    build(this, p, 0, PID_IN, 0, p.requested, &toggle, true);
    p.tds[0].status &= ~TD_SPD;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    p.qh->element = static_cast<u32>(p.tds_phys);
    return 0;
}

void UhciDevice::poll_interrupts() {
    for (int index = 16; index < 32; index++) {
        Pipe& p = pipes[index];
        if (!p.callback || gone)
            continue;
        u32 st = __atomic_load_n(&p.tds[0].status, __ATOMIC_ACQUIRE);
        if (st & TD_ACTIVE)
            continue;
        if (!(st & TD_STALLED)) {
            u32 actual = ((st & 0x7FF) + 1) & 0x7FF;
            p.toggle = !p.toggle;
            if (actual)
                p.callback(p.callback_ctx, p.buffer, actual);
        }
        bool toggle = p.toggle;
        build(this, p, 0, PID_IN, 0, p.requested, &toggle, true);
        p.tds[0].status &= ~TD_SPD;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        p.qh->element = static_cast<u32>(p.tds_phys);
    }
}

bool UhciDevice::setup() {
    ep0_max = 8;
    u8 desc[8];
    if (control(0x80, usb::REQ_GET_DESCRIPTOR, usb::DESC_DEVICE << 8, 0, desc, 8) >= 8 && desc[7])
        ep0_max = desc[7];
    u8 a = m_hc->new_address();
    if (!a || control(0x00, 5 /* SET_ADDRESS */, a, 0, nullptr, 0) < 0)
        return false;
    sched::sleep_ms(2);
    address = a;
    m_hc->devices[a] = this;
    return true;
}

usb::Device* UhciDevice::add_child(u8 hub_port, u8 child_speed) {
    auto* d = new UhciDevice(m_hc);
    d->speed = child_speed;
    d->controller = controller;
    d->hub_depth = static_cast<u8>(hub_depth + 1);
    snprintf(d->path, sizeof(d->path), "%s.%u", path, hub_port);
    if (!d->setup()) {
        d->disable();
        return nullptr;
    }
    return d;
}

void UhciDevice::disable() {
    if (gone)
        return;
    gone = true;
    for (int i = 31; i >= 0; i--) {
        Pipe& p = pipes[i];
        if (!p.qh)
            continue;
        p.callback = nullptr;
        p.qh->element = LINK_T;
        m_hc->remove(p.periodic ? m_hc->int_head : m_hc->async_head, p.qh_phys);
    }
    sched::sleep_ms(5);
    for (Pipe& p : pipes) {
        if (!p.qh)
            continue;
        pmm::free_pages(p.qh_phys, 0);
        pmm::free_pages(p.tds_phys, p.tds_order);
        pmm::free_pages(p.buffer_phys, p.buffer_order);
        p = Pipe{};
    }
    if (address) {
        LockGuard guard(m_hc->lock);
        m_hc->used[address] = false;
        m_hc->devices[address] = nullptr;
    }
}

bool Controller::init() {
    // Global reset, then controller reset.
    w16(USBINTR, 0);
    w16(USBCMD, CMD_GRESET);
    sched::sleep_ms(50);
    w16(USBCMD, 0);
    sched::sleep_ms(10);
    w16(USBCMD, CMD_HCRESET);
    u64 deadline = clock::now_ns() + 50'000'000;
    while ((r16(USBCMD) & CMD_HCRESET) && clock::now_ns() < deadline) {}
    // Skeleton: frames -> interrupt head -> ... -> async head -> ... -> end.
    u64 frames_phys = low_pages(0), qh_phys = low_pages(0);
    if (!frames_phys || !qh_phys)
        return false;
    int_head = mm::phys_to_virt<Qh>(qh_phys);
    int_head_phys = qh_phys;
    async_head = mm::phys_to_virt<Qh>(qh_phys + 64);
    async_head_phys = qh_phys + 64;
    async_head->head = LINK_T;
    async_head->element = LINK_T;
    int_head->head = static_cast<u32>(async_head_phys) | LINK_QH;
    int_head->element = LINK_T;
    frames = mm::phys_to_virt<u32>(frames_phys);
    for (int i = 0; i < 1024; i++)
        frames[i] = static_cast<u32>(int_head_phys) | LINK_QH;
    w32(FRBASEADD, static_cast<u32>(frames_phys));
    w16(FRNUM, 0);
    arch::outb(static_cast<u16>(io + SOFMOD), 64);
    w16(USBSTS, 0xFFFF);
    w16(USBCMD, CMD_RUN | CMD_MAXP64);
    sched::sleep_ms(2);
    return !(r16(USBSTS) & STS_HALTED);
}

void Controller::enumerate_port(u32 port) {
    u16 off = static_cast<u16>(PORTSC1 + 2 * (port - 1));
    u16 sc = r16(off);
    w16(off, sc | PORT_CSC | PORT_PEC); // clear changes (RW1C)
    if (!(sc & PORT_CCS))
        return;
    // Reset 50 ms, then enable.
    w16(off, PORT_RESET);
    sched::sleep_ms(50);
    w16(off, 0);
    sched::sleep_ms(1);
    for (int i = 0; i < 10 && !(r16(off) & PORT_PE); i++) {
        w16(off, PORT_PE | PORT_CSC | PORT_PEC);
        sched::sleep_ms(10);
    }
    if (!(r16(off) & PORT_PE))
        return;
    auto* d = new UhciDevice(this);
    d->speed = (r16(off) & PORT_LSDA) ? 2 : 1;
    d->controller = entry;
    snprintf(d->path, sizeof(d->path), "%u-%u", bus_number, port);
    if (!d->setup()) {
        klog::warn("uhci: device on port %u could not be addressed", port);
        d->disable();
        return;
    }
    root[port - 1] = d;
    usb::attach(d);
}

void poll_thread(void* arg) {
    auto* hc = static_cast<Controller*>(arg);
    u32 tick = 0;
    for (;;) {
        sched::sleep_ms(1);
        for (UhciDevice* d : hc->devices)
            if (d && !d->gone)
                d->poll_interrupts();
        if (++tick % 250 == 0) {
            for (u32 p = 1; p <= 2; p++) {
                u16 sc = hc->r16(static_cast<u16>(PORTSC1 + 2 * (p - 1)));
                if (!(sc & PORT_CSC))
                    continue;
                struct Work { Controller* hc; u32 port; };
                usb::queue_work([](void* a) {
                    auto* w = static_cast<Work*>(a);
                    Controller* c = w->hc;
                    u32 port = w->port;
                    delete w;
                    if (c->root[port - 1]) {
                        usb::detach(c->root[port - 1]);
                        c->root[port - 1] = nullptr;
                    }
                    sched::sleep_ms(100);
                    c->enumerate_port(port);
                }, new Work{hc, p});
            }
        }
    }
}

bool match(const dev::Device* d) { return d->bus == dev::Bus::Pci && d->class_code == 0x0C0300; }

long probe(dev::Device* d) {
    pci::Function* f = pci::function_of(d);
    pci::enable(*f);
    u32 bar4 = pci::read32(*f, 0x20);
    if (!(bar4 & 1))
        return -err::NODEV;
    // Legacy support: firmware keyboard emulation and SMIs off.
    pci::write16(*f, 0xC0, 0x8F00);
    auto* hc = new Controller;
    hc->entry = d;
    hc->io = static_cast<u16>(bar4 & ~3u);
    if (!hc->init()) {
        snprintf(d->status_text, sizeof(d->status_text), "the controller did not start");
        return -err::IO;
    }
    u32 n = 1;
    for (Controller* c = g_controllers; c; c = c->next)
        n++;
    hc->bus_number = 200 + n;
    hc->next = g_controllers;
    g_controllers = hc;
    snprintf(d->name, sizeof(d->name), "USB 1.1 host controller (UHCI, 2 ports)");
    sched::spawn("uhci-poll", poll_thread, hc, -1, true);
    return 0;
}

dev::Driver g_driver{"uhci_hcd", match, probe, {}};

} // namespace

void register_driver() { dev::register_driver(&g_driver); }

void enumerate_all() {
    for (Controller* c = g_controllers; c; c = c->next)
        for (u32 p = 1; p <= 2; p++)
            c->enumerate_port(p);
}

} // namespace uhci
