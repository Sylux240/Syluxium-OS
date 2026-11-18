// OHCI: the USB 1.1 host controller of non-Intel chipsets (AMD, NVIDIA,
// SiS, ALi, Apple, many servers) and EHCI's companion there. The controller
// walks lists of endpoint descriptors (EDs) - control, bulk, and periodic
// through the HCCA's interrupt table - each with a queue of transfer
// descriptors (TDs) ending in a dummy TD. Completion is polled every
// millisecond by watching each ED's head reach its tail.

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

namespace ohci {

namespace {

constexpr u32 HcControl = 0x04, HcCommandStatus = 0x08, HcInterruptDisable = 0x14, HcHCCA = 0x18,
              HcControlHeadED = 0x20, HcBulkHeadED = 0x28, HcFmInterval = 0x34, HcPeriodicStart = 0x40,
              HcRhDescriptorA = 0x48, HcRhStatus = 0x50, HcRhPortStatus = 0x54;
constexpr u32 CTRL_IR = 1u << 8, CTRL_PLE = 1u << 2, CTRL_CLE = 1u << 4, CTRL_BLE = 1u << 5, CTRL_OPERATIONAL = 2u << 6;
constexpr u32 CMD_HCR = 1, CMD_CLF = 2, CMD_BLF = 4, CMD_OCR = 8;
constexpr u32 PORT_CCS = 1, PORT_PES = 2, PORT_PRS = 0x10, PORT_PPS = 0x100, PORT_LSDA = 0x200, PORT_CSC = 1u << 16,
              PORT_PRSC = 1u << 20;
constexpr u32 TD_DP_SETUP = 0, TD_DP_OUT = 1u << 19, TD_DP_IN = 2u << 19;
constexpr u32 TD_TOGGLE_ED = 0, TD_DATA0 = 2u << 24, TD_DATA1 = 3u << 24, TD_ROUNDING = 1u << 18;
constexpr u32 TD_NO_INT = 7u << 21;

struct alignas(16) Ed {
    u32 control, tail, head, next;
};
struct alignas(16) Td {
    u32 control, cbp, next, be;
};

u64 low_page() {
    u64 rejected[32];
    int n = 0;
    u64 p = 0;
    for (int tries = 0; tries < 32; tries++) {
        p = pmm::alloc_page(true);
        if (!p || p + 4096 <= 0x1'0000'0000ull)
            break;
        rejected[n++] = p;
        p = 0;
    }
    while (n > 0)
        pmm::free_pages(rejected[--n], 0);
    return p;
}

class Controller;

class OhciDevice : public usb::Device {
public:
    explicit OhciDevice(Controller* hc) : m_hc(hc) {}
    Controller* m_hc;
    u8 address = 0;
    u8 ep0_max = 8;

    struct Pipe {
        Ed* ed = nullptr;
        u64 page_phys = 0;   // ED + 63 TDs (16 bytes each) in one page
        Td* tds = nullptr;
        u8* buffer = nullptr;
        u64 buffer_phys = 0;
        u16 max_packet = 8;
        u8 ep_num = 0;
        bool in = false;
        bool periodic = false;
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
    long set_hub(u8, bool, u8) override { return 0; }
    usb::Device* add_child(u8 hub_port, u8 child_speed) override;
    void disable() override;

    bool setup();
    bool make_pipe(u8 index, u8 ep_num, bool in, u16 max_packet, bool periodic);
    u32 ed_control(const Pipe& p) const {
        u32 dir = p.ep_num == 0 ? 0 : (p.in ? 2u : 1u); // 0: from the TDs (control)
        return address | (static_cast<u32>(p.ep_num) << 7) | (dir << 11) | (speed == 2 ? 1u << 13 : 0) |
               (static_cast<u32>(p.max_packet) << 16);
    }
    long run(Pipe& p, u32 count, u64 timeout_ms);
    long bulk(u8 ep_address, void* data, u32 length, u64 timeout_ms);
    void poll_interrupts();
};

class Controller {
public:
    dev::Device* entry = nullptr;
    volatile u8* regs = nullptr;
    u32 ports = 0;
    u32 bus_number = 0;
    u32* hcca = nullptr;           // 32 interrupt-table entries at the start
    Ed* periodic_head = nullptr;   // every table entry points here
    u64 periodic_head_phys = 0;
    Ed* control_head = nullptr;    // dummy heads (skipped: sKip bit)
    u64 control_head_phys = 0;
    Ed* bulk_head = nullptr;
    u64 bulk_head_phys = 0;
    sched::Mutex lock;
    bool used[128] = {};
    OhciDevice* devices[128] = {};
    OhciDevice* root[16] = {};
    Controller* next = nullptr;

    u32 reg(u32 o) { return *reinterpret_cast<volatile u32*>(regs + o); }
    void set(u32 o, u32 v) { *reinterpret_cast<volatile u32*>(regs + o) = v; }
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
    void insert(Ed* head, Ed* ed, u64 phys) {
        LockGuard guard(lock);
        ed->next = head->next;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        head->next = static_cast<u32>(phys);
    }
    void remove(Ed* head, u64 phys) {
        LockGuard guard(lock);
        Ed* prev = head;
        for (int i = 0; i < 512 && prev->next; i++) {
            if (prev->next == static_cast<u32>(phys)) {
                prev->next = mm::phys_to_virt<Ed>(phys)->next;
                break;
            }
            prev = mm::phys_to_virt<Ed>(prev->next);
        }
        sched::sleep_ms(2); // past the current frame
    }
};

Controller* g_controllers = nullptr;

bool OhciDevice::make_pipe(u8 index, u8 ep_num, bool in, u16 max_packet, bool periodic) {
    Pipe& p = pipes[index];
    if (!p.ed) {
        p.page_phys = low_page();
        p.buffer_phys = low_page();
        if (!p.page_phys || !p.buffer_phys)
            return false;
        p.ed = mm::phys_to_virt<Ed>(p.page_phys);
        p.tds = mm::phys_to_virt<Td>(p.page_phys + 16);
        p.buffer = mm::phys_to_virt<u8>(p.buffer_phys);
    }
    p.ep_num = ep_num;
    p.in = in;
    p.max_packet = max_packet;
    p.periodic = periodic;
    // Empty queue: head == tail == a dummy TD.
    u32 dummy = static_cast<u32>(p.page_phys + 16);
    p.ed->control = ed_control(p);
    p.ed->tail = dummy;
    p.ed->head = dummy;
    Controller* hc = m_hc;
    hc->insert(periodic ? hc->periodic_head : ep_num == 0 ? hc->control_head : hc->bulk_head, p.ed, p.page_phys);
    return true;
}

// TDs [0, count) are filled; append the dummy, start, wait for head == tail.
long OhciDevice::run(Pipe& p, u32 count, u64 timeout_ms) {
    u32 base = static_cast<u32>(p.page_phys + 16);
    for (u32 i = 0; i < count; i++)
        p.tds[i].next = base + (i + 1) * 16;
    Td& dummy = p.tds[count];
    memset(&dummy, 0, sizeof(dummy));
    u32 tail = base + count * 16;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    p.ed->head = base | (p.ed->head & 2); // keep the toggle carry, clear halt
    p.ed->tail = tail;
    m_hc->set(HcCommandStatus, p.ep_num == 0 ? CMD_CLF : CMD_BLF);
    u64 deadline = clock::now_ns() + timeout_ms * 1'000'000;
    for (;;) {
        if (gone)
            return -err::NODEV;
        u32 head = __atomic_load_n(&p.ed->head, __ATOMIC_ACQUIRE);
        if (head & 1) { // halted: a TD failed (or a short packet without rounding)
            klog::warn("ohci: ep %u%s halted: TD0 cc %u", p.ep_num, p.in ? " in" : "", p.tds[0].control >> 28);
            p.ed->head = tail; // drop the rest
            return -err::IO;
        }
        if ((head & ~0xFu) == tail)
            return 0;
        if (clock::now_ns() > deadline) {
            if (timeout_ms < 100000)
                klog::warn("ohci: ep %u%s timed out: head %#x tail %#x ed %#x TD0 cc %u cbp %#x be %#x", p.ep_num,
                           p.in ? " in" : "", head, tail, p.ed->control, p.tds[0].control >> 28, p.tds[0].cbp,
                           p.tds[0].be);
            p.ed->control |= 1u << 14; // sKip, then drop the queue
            sched::sleep_ms(2);
            p.ed->head = tail;
            p.ed->control &= ~(1u << 14);
            return -err::TIMEDOUT;
        }
        sched::sleep_ms(1);
    }
}

void fill(OhciDevice::Pipe& p, u32 i, u32 pid_toggle, u32 off, u32 len) {
    Td& t = p.tds[i];
    t.control = pid_toggle | TD_ROUNDING | TD_NO_INT | (0xFu << 28);
    t.cbp = len ? static_cast<u32>(p.buffer_phys + off) : 0;
    t.be = len ? static_cast<u32>(p.buffer_phys + off + len - 1) : 0;
}

// Bytes a completed TD moved: CBP 0 = all of it, else up to CBP.
u32 moved(const OhciDevice::Pipe& p, u32 i, u32 off, u32 len) {
    u32 cbp = p.tds[i].cbp;
    return cbp ? cbp - static_cast<u32>(p.buffer_phys + off) : len;
}

long OhciDevice::control(u8 type, u8 request, u16 value, u16 index, void* data, u16 length) {
    Pipe& p = pipes[0];
    if (gone)
        return -err::NODEV;
    if (!p.ed && !make_pipe(0, 0, false, ep0_max, false))
        return -err::NOMEM;
    if (length > 4096 - 8)
        return -err::INVAL;
    p.max_packet = ep0_max;
    p.ed->control = ed_control(p);
    u8 pkt[8] = {type, request, static_cast<u8>(value), static_cast<u8>(value >> 8), static_cast<u8>(index),
                 static_cast<u8>(index >> 8), static_cast<u8>(length), static_cast<u8>(length >> 8)};
    memcpy(p.buffer + 4096 - 8, pkt, 8);
    bool in = type & 0x80;
    if (!in && length)
        memcpy(p.buffer, data, length);
    u32 n = 0;
    fill(p, n++, TD_DP_SETUP | TD_DATA0, 4096 - 8, 8);
    if (length)
        fill(p, n++, (in ? TD_DP_IN : TD_DP_OUT) | TD_DATA1, 0, length);
    fill(p, n++, ((in && length) ? TD_DP_OUT : TD_DP_IN) | TD_DATA1, 0, 0);
    if (long r = run(p, n, 2000); r < 0)
        return r;
    u32 actual = length ? moved(p, 1, 0, length) : 0;
    if (in && actual)
        memcpy(data, p.buffer, actual);
    return actual;
}

long OhciDevice::open_endpoint(const usb::Endpoint& e) {
    u8 index = static_cast<u8>((e.address & 0xF) + (e.is_in() ? 16 : 0));
    if (pipes[index].ed)
        return 0;
    return make_pipe(index, e.address & 0xF, e.is_in(), e.max_packet ? e.max_packet : 8, e.type() == 3) ? 0
                                                                                                          : -err::NOMEM;
}

long OhciDevice::bulk(u8 ep_address, void* data, u32 length, u64 timeout_ms) {
    u8 index = static_cast<u8>((ep_address & 0xF) + ((ep_address & 0x80) ? 16 : 0));
    Pipe& p = pipes[index];
    if (gone)
        return -err::NODEV;
    if (!p.ed || p.periodic)
        return -err::INVAL;
    bool in = ep_address & 0x80;
    u32 done = 0;
    do {
        u32 chunk = min<u32>(length - done, 4096); // one page per round (a TD may cross one page boundary)
        if (!in)
            memcpy(p.buffer, static_cast<u8*>(data) + done, chunk);
        fill(p, 0, (in ? TD_DP_IN : TD_DP_OUT) | TD_TOGGLE_ED, 0, chunk);
        if (long r = run(p, 1, timeout_ms); r < 0)
            return r;
        u32 got = moved(p, 0, 0, chunk);
        if (in)
            memcpy(static_cast<u8*>(data) + done, p.buffer, got);
        done += got;
        if (got < chunk)
            break;
    } while (done < length);
    return done;
}

long OhciDevice::start_interrupt_in(u8 ep_address, u32 length, usb::InterruptCallback cb, void* ctx) {
    u8 index = static_cast<u8>((ep_address & 0xF) + 16);
    Pipe& p = pipes[index];
    if (!p.ed || !p.periodic)
        return -err::INVAL;
    p.requested = min<u32>(length, 1024);
    p.callback_ctx = ctx;
    p.callback = cb;
    fill(p, 0, TD_DP_IN | TD_TOGGLE_ED, 0, p.requested);
    u32 base = static_cast<u32>(p.page_phys + 16);
    p.tds[0].next = base + 16;
    memset(&p.tds[1], 0, sizeof(Td));
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    p.ed->head = base | (p.ed->head & 2);
    p.ed->tail = base + 16;
    return 0;
}

void OhciDevice::poll_interrupts() {
    for (int index = 16; index < 32; index++) {
        Pipe& p = pipes[index];
        if (!p.callback || gone)
            continue;
        u32 base = static_cast<u32>(p.page_phys + 16);
        u32 head = __atomic_load_n(&p.ed->head, __ATOMIC_ACQUIRE);
        if (!(head & 1) && (head & ~0xFu) != base + 16)
            continue; // still pending
        if (!(head & 1)) {
            u32 got = moved(p, 0, 0, p.requested);
            if (got)
                p.callback(p.callback_ctx, p.buffer, got);
        }
        fill(p, 0, TD_DP_IN | TD_TOGGLE_ED, 0, p.requested);
        p.tds[0].next = base + 16;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        p.ed->head = base | (head & 2);
    }
}

bool OhciDevice::setup() {
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
    if (pipes[0].ed)
        pipes[0].ed->control = ed_control(pipes[0]);
    return true;
}

usb::Device* OhciDevice::add_child(u8 hub_port, u8 child_speed) {
    auto* d = new OhciDevice(m_hc);
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

void OhciDevice::disable() {
    if (gone)
        return;
    gone = true;
    for (int i = 31; i >= 0; i--) {
        Pipe& p = pipes[i];
        if (!p.ed)
            continue;
        p.callback = nullptr;
        p.ed->control |= 1u << 14;
        m_hc->remove(p.periodic ? m_hc->periodic_head : p.ep_num == 0 ? m_hc->control_head : m_hc->bulk_head,
                     p.page_phys);
    }
    sched::sleep_ms(5);
    for (Pipe& p : pipes) {
        if (!p.ed)
            continue;
        pmm::free_pages(p.page_phys, 0);
        pmm::free_pages(p.buffer_phys, 0);
        p = Pipe{};
    }
    if (address) {
        LockGuard guard(m_hc->lock);
        m_hc->used[address] = false;
        m_hc->devices[address] = nullptr;
    }
}

bool Controller::init() {
    // From the firmware (SMM) if it owns it.
    if (reg(HcControl) & CTRL_IR) {
        set(HcCommandStatus, CMD_OCR);
        u64 deadline = clock::now_ns() + 1'000'000'000;
        while ((reg(HcControl) & CTRL_IR) && clock::now_ns() < deadline)
            sched::sleep_ms(1);
    }
    set(HcInterruptDisable, 0xC000007F); // completion is polled
    u32 fm = reg(HcFmInterval);
    set(HcCommandStatus, CMD_HCR);
    u64 deadline = clock::now_ns() + 10'000'000;
    while ((reg(HcCommandStatus) & CMD_HCR) && clock::now_ns() < deadline) {}
    u64 hcca_phys = low_page(), eds_phys = low_page();
    if (!hcca_phys || !eds_phys)
        return false;
    hcca = mm::phys_to_virt<u32>(hcca_phys);
    periodic_head = mm::phys_to_virt<Ed>(eds_phys);
    periodic_head_phys = eds_phys;
    control_head = mm::phys_to_virt<Ed>(eds_phys + 16);
    control_head_phys = eds_phys + 16;
    bulk_head = mm::phys_to_virt<Ed>(eds_phys + 32);
    bulk_head_phys = eds_phys + 32;
    Ed* heads[3] = {periodic_head, control_head, bulk_head};
    for (Ed* e : heads) { // skipped dummies
        e->control = 1u << 14;
        e->next = 0;
    }
    for (int i = 0; i < 32; i++)
        hcca[i] = static_cast<u32>(periodic_head_phys);
    set(HcHCCA, static_cast<u32>(hcca_phys));
    set(HcControlHeadED, static_cast<u32>(control_head_phys));
    set(HcBulkHeadED, static_cast<u32>(bulk_head_phys));
    u32 fi = fm & 0x3FFF ? fm & 0x3FFF : 11999;
    set(HcFmInterval, ((fm & 0x80000000) ^ 0x80000000) | (((fi - 210) * 6 / 7) << 16) | fi);
    set(HcPeriodicStart, fi * 9 / 10);
    set(HcControl, CTRL_OPERATIONAL | CTRL_PLE | CTRL_CLE | CTRL_BLE | 3);
    sched::sleep_ms(2);
    ports = reg(HcRhDescriptorA) & 0xFF;
    if (ports > 15)
        ports = 15;
    set(HcRhStatus, 1u << 16); // set global power
    for (u32 i = 0; i < ports; i++)
        set(HcRhPortStatus + 4 * i, PORT_PPS);
    sched::sleep_ms(((reg(HcRhDescriptorA) >> 24) & 0xFF) * 2 + 20);
    return true;
}

void Controller::enumerate_port(u32 port) {
    u32 off = HcRhPortStatus + 4 * (port - 1);
    set(off, PORT_CSC); // clear the change
    if (!(reg(off) & PORT_CCS))
        return;
    set(off, PORT_PRS); // reset (the controller times it)
    u64 deadline = clock::now_ns() + 100'000'000;
    while (!(reg(off) & PORT_PRSC) && clock::now_ns() < deadline)
        sched::sleep_ms(1);
    set(off, PORT_PRSC);
    sched::sleep_ms(10);
    if (!(reg(off) & PORT_PES))
        return;
    auto* d = new OhciDevice(this);
    d->speed = (reg(off) & PORT_LSDA) ? 2 : 1;
    d->controller = entry;
    snprintf(d->path, sizeof(d->path), "%u-%u", bus_number, port);
    if (!d->setup()) {
        klog::warn("ohci: device on port %u could not be addressed", port);
        d->disable();
        return;
    }
    root[port] = d;
    usb::attach(d);
}

void poll_thread(void* arg) {
    auto* hc = static_cast<Controller*>(arg);
    u32 tick = 0;
    for (;;) {
        sched::sleep_ms(1);
        for (OhciDevice* d : hc->devices)
            if (d && !d->gone)
                d->poll_interrupts();
        if (++tick % 250 == 0) {
            for (u32 p = 1; p <= hc->ports; p++) {
                if (!(hc->reg(HcRhPortStatus + 4 * (p - 1)) & PORT_CSC))
                    continue;
                struct Work { Controller* hc; u32 port; };
                usb::queue_work([](void* a) {
                    auto* w = static_cast<Work*>(a);
                    Controller* c = w->hc;
                    u32 port = w->port;
                    delete w;
                    if (c->root[port]) {
                        usb::detach(c->root[port]);
                        c->root[port] = nullptr;
                    }
                    sched::sleep_ms(100);
                    c->enumerate_port(port);
                }, new Work{hc, p});
            }
        }
    }
}

bool match(const dev::Device* d) { return d->bus == dev::Bus::Pci && d->class_code == 0x0C0310; }

long probe(dev::Device* d) {
    pci::Function* f = pci::function_of(d);
    pci::enable(*f);
    auto* regs = static_cast<volatile u8*>(pci::map_bar(*f, 0));
    if (!regs)
        return -err::IO;
    auto* hc = new Controller;
    hc->entry = d;
    hc->regs = regs;
    if (!hc->init()) {
        snprintf(d->status_text, sizeof(d->status_text), "the controller did not start");
        return -err::IO;
    }
    u32 n = 1;
    for (Controller* c = g_controllers; c; c = c->next)
        n++;
    hc->bus_number = 300 + n;
    hc->next = g_controllers;
    g_controllers = hc;
    snprintf(d->name, sizeof(d->name), "USB 1.1 host controller (OHCI, %u ports)", hc->ports);
    sched::spawn("ohci-poll", poll_thread, hc, -1, true);
    return 0;
}

dev::Driver g_driver{"ohci_hcd", match, probe, {}};

} // namespace

void register_driver() { dev::register_driver(&g_driver); }

void enumerate_all() {
    for (Controller* c = g_controllers; c; c = c->next)
        for (u32 p = 1; p <= c->ports; p++)
            c->enumerate_port(p);
}

} // namespace ohci
