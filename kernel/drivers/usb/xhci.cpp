// xHCI (USB 3) host controller driver.
//
// Command ring, one event ring (serviced by a polling thread), a transfer
// ring per endpoint, devices on root ports and behind hubs. Handles low,
// full, high and super speed devices.
//
// Hot-plug: Port Status Change events mark root ports as changed; the USB
// core's worker thread then enumerates new devices or detaches removed ones
// (Controller::port_work). Removing a device disables its slot and frees its
// rings once the event thread can no longer be using them (UsbDevice::disable).

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

namespace xhci {

namespace {

struct Trb {
    u64 param;
    u32 status;
    u32 control;
};
static_assert(sizeof(Trb) == 16);

constexpr u32 TRB_NOOP = 8;
constexpr u32 TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_LINK = 6, TRB_ENABLE_SLOT = 9,
              TRB_DISABLE_SLOT = 10, TRB_ADDRESS_DEVICE = 11, TRB_CONFIGURE_EP = 12, TRB_EVALUATE_CTX = 13,
              TRB_TRANSFER_EVENT = 32, TRB_COMMAND_COMPLETION = 33, TRB_PORT_STATUS_CHANGE = 34;
constexpr u32 CC_SUCCESS = 1, CC_SHORT_PACKET = 13;
constexpr u32 CC_DEVICE_GONE = 0x100; // ours, not the controller's: the device was unplugged

// PORTSC bits.
constexpr u32 PORTSC_CCS = 1u << 0, PORTSC_PED = 1u << 1, PORTSC_CSC = 1u << 17;
constexpr u32 PORTSC_CHANGES = 0x00FE0000; // CSC PEC WRC OCC PRC PLC CEC: write 1 to clear
constexpr u32 PORTSC_RW1C = PORTSC_CHANGES | PORTSC_PED; // writing 1 to PED disables the port
constexpr u32 RING_SIZE = 256; // TRBs per ring (one page); the last is a link TRB

struct Ring {
    Trb* trbs = nullptr;
    u64 phys = 0;
    u32 enqueue = 0;
    u32 cycle = 1;

    void free() {
        if (phys)
            pmm::free_page(phys);
        phys = 0;
        trbs = nullptr;
    }

    bool init() {
        phys = pmm::alloc_page(true);
        if (!phys)
            return false;
        trbs = mm::phys_to_virt<Trb>(phys);
        trbs[RING_SIZE - 1].param = phys;
        trbs[RING_SIZE - 1].control = (TRB_LINK << 10) | (1 << 1); // toggle cycle
        return true;
    }

    // Make sure the next `n` TRBs don't straddle the link TRB (a transfer
    // descriptor must not wrap); pads with no-op TRBs.
    void reserve(u32 n) {
        while (enqueue + n > RING_SIZE - 1)
            push(0, 0, TRB_NOOP << 10);
    }

    // Append a TRB; returns its physical address (how events refer to it).
    u64 push(u64 param, u32 status, u32 control) {
        Trb& t = trbs[enqueue];
        t.param = param;
        t.status = status;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        t.control = (control & ~1u) | cycle; // cycle bit last: hands it to the controller
        u64 addr = phys + enqueue * sizeof(Trb);
        if (++enqueue == RING_SIZE - 1) {
            trbs[RING_SIZE - 1].control = (trbs[RING_SIZE - 1].control & ~1u) | cycle;
            cycle ^= 1;
            enqueue = 0;
        }
        return addr;
    }
};

class Controller;

struct EndpointState {
    Ring ring;
    u8* buffer = nullptr;   // DMA bounce buffer
    u64 buffer_phys = 0;
    u32 buffer_size = 0;
    unsigned buffer_order = 0;
    u32 requested = 0;
    // Completion of the outstanding synchronous request.
    volatile u64 wait_trb = 0;
    volatile bool done = false;
    volatile u32 code = 0;
    volatile u32 residual = 0;
    // Interrupt IN polling.
    usb::InterruptCallback callback = nullptr;
    void* callback_ctx = nullptr;
};

class UsbDevice : public usb::Device {
public:
    UsbDevice(Controller* hc, u8 slot, u8 port) : m_hc(hc), m_slot(slot), m_port(port) {}
    bool setup();

    long set_hub(u8 ports, bool multi_tt, u8 think_time) override;
    usb::Device* add_child(u8 hub_port, u8 child_speed) override;

    // Topology (slot context): route string below the root port, and the
    // high-speed hub whose transaction translator serves a low/full-speed device.
    u32 route = 0;
    u8 depth = 0;
    u8 tt_slot = 0, tt_port = 0;
    bool is_hub = false;

    long control(u8 type, u8 request, u16 value, u16 index, void* data, u16 length) override;
    long open_endpoint(const usb::Endpoint& ep) override;
    long transfer(u8 ep_address, void* data, u32 length) override;
    long start_interrupt_in(u8 ep_address, u32 length, usb::InterruptCallback cb, void* ctx) override;
    void disable() override;

    void on_transfer_event(u8 dci, u64 trb, u32 code, u32 residual);

private:
    u8* in_ctx(u32 index);   // input context: 0 = control, 1 = slot, dci+1 = endpoint
    u8* out_ctx(u32 index);  // device context: 0 = slot, dci = endpoint
    bool alloc_endpoint(u8 dci, u32 buffer_size);
    long wait(EndpointState& ep, u64 timeout_ms);
    void queue_interrupt(u8 dci);

    Controller* m_hc;
    u8 m_slot, m_port;
    u64 m_in_phys = 0, m_out_phys = 0;
    u8* m_in = nullptr;
    u8* m_out = nullptr;
    EndpointState m_eps[32];
    sched::Mutex m_ep0_lock;
    u32 m_active = 0; // requests in progress (disable() waits for them before freeing buffers)

    // RAII: counts a request as active; `ok` is false once the device is gone.
    struct Active {
        UsbDevice* d;
        bool ok;
        explicit Active(UsbDevice* dev) : d(dev) {
            __atomic_add_fetch(&d->m_active, 1, __ATOMIC_ACQ_REL);
            ok = !d->gone;
        }
        ~Active() { __atomic_sub_fetch(&d->m_active, 1, __ATOMIC_ACQ_REL); }
    };
};

class Controller {
public:
    volatile u8* cap = nullptr;
    volatile u8* op = nullptr;
    volatile u8* rt = nullptr;
    volatile u32* doorbells = nullptr;
    u32 max_slots = 0, max_ports = 0;
    u32 ctx_size = 32;
    u64* dcbaa = nullptr;
    Ring commands;
    Trb* events = nullptr;
    u64 events_phys = 0;
    u32 event_dequeue = 0, event_cycle = 1;
    UsbDevice* slots[256] = {};
    UsbDevice* root_devices[256] = {}; // device on each root port (index = port number)
    u64 pending_ports[4] = {};         // root ports with a status change to look at
    bool port_work_queued = false;
    volatile u64 event_passes = 0;     // event-thread loops completed (a grace period for teardown)
    dev::Device* entry = nullptr;
    u32 bus_number = 1;
    Controller* next = nullptr;

    // Outstanding command.
    sched::Mutex command_lock;
    volatile u64 command_trb = 0;
    volatile bool command_done = false;
    volatile u32 command_code = 0, command_slot = 0;
    sched::WaitQueue completions; // woken after every batch of events

    u32 op32(u32 off) { return *reinterpret_cast<volatile u32*>(op + off); }
    void set_op32(u32 off, u32 v) { *reinterpret_cast<volatile u32*>(op + off) = v; }
    void set_op64(u32 off, u64 v) {
        *reinterpret_cast<volatile u32*>(op + off) = static_cast<u32>(v);
        *reinterpret_cast<volatile u32*>(op + off + 4) = static_cast<u32>(v >> 32);
    }
    volatile u32* portsc(u32 port) { return reinterpret_cast<volatile u32*>(op + 0x400 + 0x10 * (port - 1)); }

    bool init();
    long command(u64 param, u32 status, u32 control, u32* slot_out, u64 timeout_ms = 2000);
    void process_events();
    void scan_ports();
    bool reset_port(u32 port);
    void enumerate_port(u32 port, bool hotplug);
    void port_changed(u32 port);
    static void port_work(void* arg);
    void wait_event_pass();
};

// ---- controller ----

bool wait_for(volatile u32* reg, u32 mask, u32 value, u64 timeout_ms) {
    u64 deadline = clock::now_ns() + timeout_ms * 1'000'000;
    while ((*reg & mask) != value)
        if (clock::now_ns() > deadline)
            return false;
    return true;
}

bool Controller::init() {
    u8 caplength = cap[0];
    u32 hcs1 = *reinterpret_cast<volatile u32*>(cap + 4);
    u32 hcs2 = *reinterpret_cast<volatile u32*>(cap + 8);
    u32 hcc1 = *reinterpret_cast<volatile u32*>(cap + 0x10);
    u32 dboff = *reinterpret_cast<volatile u32*>(cap + 0x14) & ~3u;
    u32 rtsoff = *reinterpret_cast<volatile u32*>(cap + 0x18) & ~0x1Fu;
    op = cap + caplength;
    rt = cap + rtsoff;
    doorbells = reinterpret_cast<volatile u32*>(cap + dboff);
    max_slots = min<u32>(hcs1 & 0xFF, 64);
    max_ports = (hcs1 >> 24) & 0xFF;
    ctx_size = (hcc1 & (1 << 2)) ? 64 : 32;

    // Take the controller from the BIOS (USB legacy support capability).
    for (u32 off = ((hcc1 >> 16) & 0xFFFF) * 4; off;) {
        volatile u32* ext = reinterpret_cast<volatile u32*>(cap + off);
        if ((*ext & 0xFF) == 1) {
            *ext |= 1u << 24;                     // OS owned
            wait_for(ext, 1u << 16, 0, 1000);     // BIOS releases
        }
        u32 step = (*ext >> 8) & 0xFF;
        off = step ? off + step * 4 : 0;
    }

    // Stop, then reset.
    set_op32(0x00, op32(0x00) & ~1u);
    if (!wait_for(reinterpret_cast<volatile u32*>(op + 0x04), 1, 1, 1000))
        return false;
    set_op32(0x00, 1u << 1);
    if (!wait_for(reinterpret_cast<volatile u32*>(op + 0x00), 1u << 1, 0, 1000) ||
        !wait_for(reinterpret_cast<volatile u32*>(op + 0x04), 1u << 11, 0, 1000))
        return false;

    set_op32(0x38, max_slots); // CONFIG: slots enabled

    u64 dcbaa_phys = pmm::alloc_page(true);
    if (!dcbaa_phys)
        return false;
    dcbaa = mm::phys_to_virt<u64>(dcbaa_phys);

    // Scratchpad buffers the controller asks for (its private memory).
    u32 scratch = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (scratch) {
        u64 array_phys = pmm::alloc_page(true);
        u64* array = mm::phys_to_virt<u64>(array_phys);
        for (u32 i = 0; i < scratch && i < 512; i++)
            array[i] = pmm::alloc_page(true);
        dcbaa[0] = array_phys;
    }
    set_op64(0x30, dcbaa_phys);

    if (!commands.init())
        return false;
    set_op64(0x18, commands.phys | 1); // CRCR, ring cycle state 1

    // Event ring: one segment, described by a one-entry segment table.
    events_phys = pmm::alloc_page(true);
    u64 erst_phys = pmm::alloc_page(true);
    if (!events_phys || !erst_phys)
        return false;
    events = mm::phys_to_virt<Trb>(events_phys);
    u64* erst = mm::phys_to_virt<u64>(erst_phys);
    erst[0] = events_phys;
    erst[1] = RING_SIZE;
    volatile u8* ir0 = rt + 0x20;
    *reinterpret_cast<volatile u32*>(ir0 + 0x08) = 1; // ERSTSZ
    *reinterpret_cast<volatile u64*>(ir0 + 0x18) = events_phys; // ERDP
    *reinterpret_cast<volatile u64*>(ir0 + 0x10) = erst_phys;   // ERSTBA (last)

    set_op32(0x00, 1); // run
    return wait_for(reinterpret_cast<volatile u32*>(op + 0x04), 1, 0, 1000);
}

void Controller::process_events() {
    bool any = false;
    for (;;) {
        Trb& e = events[event_dequeue];
        if ((e.control & 1) != event_cycle)
            break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        u32 type = (e.control >> 10) & 0x3F;
        u32 code = e.status >> 24;
        if (type == TRB_COMMAND_COMPLETION) {
            if (e.param == command_trb) {
                command_code = code;
                command_slot = e.control >> 24;
                command_done = true;
            }
        } else if (type == TRB_TRANSFER_EVENT) {
            u8 slot = static_cast<u8>(e.control >> 24);
            u8 dci = (e.control >> 16) & 0x1F;
            if (slots[slot])
                slots[slot]->on_transfer_event(dci, e.param, code, e.status & 0xFFFFFF);
        }
        else if (type == TRB_PORT_STATUS_CHANGE) {
            port_changed(static_cast<u32>(e.param >> 24) & 0xFF);
        }
        any = true;
        if (++event_dequeue == RING_SIZE) {
            event_dequeue = 0;
            event_cycle ^= 1;
        }
    }
    if (any) {
        volatile u8* ir0 = rt + 0x20;
        *reinterpret_cast<volatile u64*>(ir0 + 0x18) = (events_phys + event_dequeue * sizeof(Trb)) | (1 << 3);
        completions.wake_all();
    }
}

long Controller::command(u64 param, u32 status, u32 control, u32* slot_out, u64 timeout_ms) {
    LockGuard guard(command_lock);
    command_done = false;
    command_trb = commands.push(param, status, control);
    doorbells[0] = 0;
    completions.wait_until_timeout([&] { return command_done; }, timeout_ms * 1'000'000);
    if (!command_done)
        return -err::TIMEDOUT;
    if (slot_out)
        *slot_out = command_slot;
    return command_code == CC_SUCCESS ? 0 : -err::IO;
}

bool Controller::reset_port(u32 port) {
    volatile u32* sc = portsc(port);
    constexpr u32 RW1C = 0x00FE0002; // change bits + PED: writing 1 clears/disables
    *sc = (*sc & ~RW1C) | (1u << 4);  // port reset
    u64 deadline = clock::now_ns() + 500'000'000;
    while (!(*sc & (1u << 21))) // reset change
        if (clock::now_ns() > deadline)
            return false;
    *sc = (*sc & ~RW1C) | (1u << 21); // acknowledge
    sched::sleep_ms(10);
    return *sc & (1u << 1); // enabled
}

void Controller::wait_event_pass() {
    u64 start = __atomic_load_n(&event_passes, __ATOMIC_ACQUIRE);
    // Two completed passes: any process_events() that could still have seen
    // an old slots[] entry has finished.
    while (__atomic_load_n(&event_passes, __ATOMIC_ACQUIRE) < start + 2)
        sched::sleep_ms(1);
}

// Event thread: remember the port and let the USB worker look at it.
void Controller::port_changed(u32 port) {
    if (port == 0 || port > max_ports)
        return;
    __atomic_or_fetch(&pending_ports[port / 64], 1ull << (port % 64), __ATOMIC_RELEASE);
    if (!__atomic_exchange_n(&port_work_queued, true, __ATOMIC_ACQ_REL))
        usb::queue_work(port_work, this);
}

// USB worker (topology lock held): connects and disconnects on root ports.
void Controller::port_work(void* arg) {
    auto* hc = static_cast<Controller*>(arg);
    __atomic_store_n(&hc->port_work_queued, false, __ATOMIC_RELEASE); // later events queue us again
    for (u32 port = 1; port <= hc->max_ports; port++) {
        u64 bit = 1ull << (port % 64);
        if (!(__atomic_fetch_and(&hc->pending_ports[port / 64], ~bit, __ATOMIC_ACQ_REL) & bit))
            continue;
        volatile u32* reg = hc->portsc(port);
        u32 sc = *reg;
        u32 changes = sc & PORTSC_CHANGES;
        if (changes)
            *reg = (sc & ~PORTSC_RW1C) | changes; // acknowledge
        bool connected = sc & PORTSC_CCS;
        UsbDevice* d = hc->root_devices[port];
        // Unplugged - or unplugged and replugged before we looked (a connect
        // change on a port that still has a device).
        if (d && (!connected || (changes & PORTSC_CSC))) {
            usb::detach(d);
            hc->root_devices[port] = nullptr;
        }
        if (connected && !hc->root_devices[port])
            hc->enumerate_port(port, true);
    }
}

void Controller::enumerate_port(u32 port, bool hotplug) {
    volatile u32* reg = portsc(port);
    u32 sc = *reg;
    // This connection is being handled now: clear its change bits so a later
    // event on the port doesn't mistake them for an unplug + replug.
    if (sc & PORTSC_CHANGES)
        *reg = (sc & ~PORTSC_RW1C) | (sc & PORTSC_CHANGES);
    if (!(sc & PORTSC_CCS))
        return;
    if (hotplug) {
        sched::sleep_ms(100); // USB 2.0 7.1.7.3: let the connection settle (debounce)
        if (!(*reg & PORTSC_CCS))
            return;
    }
    // USB 3 ports enable themselves; USB 2 ports need a reset first.
    if (!(*reg & PORTSC_PED) && !reset_port(port))
        return;
    u32 slot;
    if (command(0, 0, TRB_ENABLE_SLOT << 10, &slot) != 0 || !slot || slot >= 256)
        return;
    auto* d = new UsbDevice(this, static_cast<u8>(slot), static_cast<u8>(port));
    d->speed = static_cast<u8>((*reg >> 10) & 0xF);
    d->controller = entry;
    snprintf(d->path, sizeof(d->path), "%u-%u", bus_number, port);
    slots[slot] = d;
    if (!d->setup()) {
        klog::warn("xhci: device on port %u could not be addressed", port);
        d->disable(); // gives the slot back
        return;
    }
    root_devices[port] = d;
    usb::attach(d);
}

void Controller::scan_ports() {
    for (u32 port = 1; port <= max_ports; port++)
        enumerate_port(port, false);
}

// ---- device ----

u8* UsbDevice::in_ctx(u32 index) { return m_in + index * m_hc->ctx_size; }
u8* UsbDevice::out_ctx(u32 index) { return m_out + index * m_hc->ctx_size; }

bool UsbDevice::alloc_endpoint(u8 dci, u32 buffer_size) {
    EndpointState& ep = m_eps[dci];
    if (!ep.ring.init())
        return false;
    unsigned order = pmm::order_for_size(buffer_size);
    ep.buffer_phys = pmm::alloc_pages(order, true); // naturally aligned: never crosses 64 KiB
    if (!ep.buffer_phys)
        return false;
    ep.buffer = mm::phys_to_virt<u8>(ep.buffer_phys);
    ep.buffer_size = 4096u << order;
    ep.buffer_order = order;
    return true;
}

bool UsbDevice::setup() {
    m_in_phys = pmm::alloc_page(true);
    m_out_phys = pmm::alloc_page(true);
    if (!m_in_phys || !m_out_phys || !alloc_endpoint(1, 4096))
        return false;
    m_in = mm::phys_to_virt<u8>(m_in_phys);
    m_out = mm::phys_to_virt<u8>(m_out_phys);
    m_hc->dcbaa[m_slot] = m_out_phys;

    u16 mps = speed == 4 || speed == 5 ? 512 : speed == 3 ? 64 : 8;
    auto* icc = reinterpret_cast<u32*>(in_ctx(0));
    icc[1] = 0x3; // add slot context + EP0
    auto* slot = reinterpret_cast<u32*>(in_ctx(1));
    slot[0] = (1u << 27) | (static_cast<u32>(speed) << 20) | route; // one context entry, speed, route
    slot[1] = static_cast<u32>(m_port) << 16;                       // root hub port
    slot[2] = static_cast<u32>(tt_slot) | (static_cast<u32>(tt_port) << 8);
    auto* ep0 = reinterpret_cast<u32*>(in_ctx(2));
    ep0[1] = (3u << 1) | (4u << 3) | (static_cast<u32>(mps) << 16); // CErr 3, control, max packet
    ep0[2] = static_cast<u32>(m_eps[1].ring.phys) | 1;              // dequeue pointer + cycle
    ep0[3] = static_cast<u32>(m_eps[1].ring.phys >> 32);
    ep0[4] = 8; // average TRB length
    if (m_hc->command(m_in_phys, 0, (TRB_ADDRESS_DEVICE << 10) | (static_cast<u32>(m_slot) << 24), nullptr) != 0)
        return false;

    // Full-speed devices: learn EP0's real max packet size, then update it.
    if (speed == 1) {
        u8 head[8];
        if (control(0x80, usb::REQ_GET_DESCRIPTOR, usb::DESC_DEVICE << 8, 0, head, 8) == 8 && head[7] != 8) {
            memset(m_in, 0, 4096);
            icc[1] = 0x2; // evaluate EP0
            ep0[1] = (3u << 1) | (4u << 3) | (static_cast<u32>(head[7]) << 16);
            m_hc->command(m_in_phys, 0, (TRB_EVALUATE_CTX << 10) | (static_cast<u32>(m_slot) << 24), nullptr);
        }
    }
    return true;
}

long UsbDevice::set_hub(u8 ports, bool multi_tt, u8 think_time) {
    is_hub = true;
    memset(m_in, 0, 4096);
    auto* icc = reinterpret_cast<u32*>(in_ctx(0));
    icc[1] = 1; // slot context only
    memcpy(in_ctx(1), out_ctx(0), m_hc->ctx_size);
    auto* slot = reinterpret_cast<u32*>(in_ctx(1));
    slot[0] |= 1u << 26;                                  // Hub
    if (multi_tt)
        slot[0] |= 1u << 25;                              // MTT
    slot[1] = (slot[1] & 0x00FFFFFF) | (static_cast<u32>(ports) << 24);
    slot[2] = (slot[2] & ~(3u << 16)) | (static_cast<u32>(think_time & 3) << 16);
    slot[3] = 0;
    return m_hc->command(m_in_phys, 0, (TRB_CONFIGURE_EP << 10) | (static_cast<u32>(m_slot) << 24), nullptr);
}

usb::Device* UsbDevice::add_child(u8 hub_port, u8 child_speed) {
    if (depth >= 5)
        return nullptr; // USB allows 5 tiers of hubs
    u32 slot_id;
    if (m_hc->command(0, 0, TRB_ENABLE_SLOT << 10, &slot_id) != 0 || !slot_id || slot_id >= 256)
        return nullptr;
    auto* d = new UsbDevice(m_hc, static_cast<u8>(slot_id), m_port);
    d->speed = child_speed;
    d->controller = controller;
    d->depth = static_cast<u8>(depth + 1);
    d->hub_depth = d->depth;
    d->route = route | (static_cast<u32>(hub_port < 15 ? hub_port : 15) << (4 * depth));
    snprintf(d->path, sizeof(d->path), "%s.%u", path, hub_port);
    // Low/full-speed device: the nearest high-speed hub translates for it.
    if (child_speed == 1 || child_speed == 2) {
        if (speed == 3) {
            d->tt_slot = m_slot;
            d->tt_port = hub_port;
        } else {
            d->tt_slot = tt_slot;
            d->tt_port = tt_port;
        }
    }
    m_hc->slots[slot_id] = d;
    if (!d->setup()) {
        d->disable(); // gives the slot back
        return nullptr;
    }
    return d;
}

long UsbDevice::wait(EndpointState& ep, u64 timeout_ms) {
    m_hc->completions.wait_until_timeout([&] { return ep.done || gone; }, timeout_ms * 1'000'000);
    if (gone)
        return -err::NODEV;
    if (!ep.done)
        return -err::TIMEDOUT;
    if (ep.code == CC_DEVICE_GONE)
        return -err::NODEV;
    if (ep.code != CC_SUCCESS && ep.code != CC_SHORT_PACKET)
        return -err::IO;
    return 0;
}

void UsbDevice::on_transfer_event(u8 dci, u64 trb, u32 code, u32 residual) {
    if (dci >= 32 || gone)
        return;
    EndpointState& ep = m_eps[dci];
    if (ep.callback) {
        if (code == CC_SUCCESS || code == CC_SHORT_PACKET)
            ep.callback(ep.callback_ctx, ep.buffer, ep.requested - residual);
        queue_interrupt(dci);
        return;
    }
    // The awaited TRB completed, or an error stopped the endpoint before it.
    if (trb == ep.wait_trb || (code != CC_SUCCESS && code != CC_SHORT_PACKET)) {
        ep.code = code;
        ep.residual = residual;
        ep.done = true;
    }
}

long UsbDevice::control(u8 type, u8 request, u16 value, u16 index, void* data, u16 length) {
    Active active(this);
    if (!active.ok)
        return -err::NODEV;
    LockGuard guard(m_ep0_lock);
    EndpointState& ep = m_eps[1];
    bool in = type & 0x80;
    if (length > ep.buffer_size)
        return -err::INVAL;
    if (!in && length)
        memcpy(ep.buffer, data, length);
    u64 setup = type | (static_cast<u64>(request) << 8) | (static_cast<u64>(value) << 16) |
                (static_cast<u64>(index) << 32) | (static_cast<u64>(length) << 48);
    u32 trt = length ? (in ? 3 : 2) : 0;
    ep.done = false;
    ep.wait_trb = ~0ull;
    ep.ring.reserve(3);
    ep.ring.push(setup, 8, (TRB_SETUP << 10) | (1 << 6) | (trt << 16)); // immediate data
    if (length)
        ep.ring.push(ep.buffer_phys, length, (TRB_DATA << 10) | (in ? (1u << 16) : 0));
    u32 status_dir = (in && length) ? 0 : (1u << 16);
    ep.wait_trb = ep.ring.push(0, 0, (TRB_STATUS << 10) | (1 << 5) | status_dir); // interrupt on completion
    m_hc->doorbells[m_slot] = 1;
    if (long r = wait(ep, 2000); r < 0)
        return r;
    if (in && length)
        memcpy(data, ep.buffer, length);
    return length;
}

long UsbDevice::open_endpoint(const usb::Endpoint& e) {
    u8 num = e.address & 0xF;
    u8 dci = static_cast<u8>(num * 2 + (e.is_in() ? 1 : 0));
    if (!alloc_endpoint(dci, e.type() == 2 ? 64 * 1024 : 4096))
        return -err::NOMEM;

    memset(m_in, 0, 4096);
    auto* icc = reinterpret_cast<u32*>(in_ctx(0));
    icc[1] = 1u | (1u << dci); // slot + this endpoint
    // Slot context: copy the current one, raise "context entries" if needed.
    memcpy(in_ctx(1), out_ctx(0), m_hc->ctx_size);
    auto* slot = reinterpret_cast<u32*>(in_ctx(1));
    u32 entries = (slot[0] >> 27) & 0x1F;
    if (dci > entries)
        slot[0] = (slot[0] & ~(0x1Fu << 27)) | (static_cast<u32>(dci) << 27);
    slot[3] = 0;

    u32 ep_type = e.type() == 2 ? (e.is_in() ? 6 : 2) : e.type() == 3 ? (e.is_in() ? 7 : 3) : (e.is_in() ? 5 : 1);
    u32 interval = 0;
    if (e.type() == 3) {
        if (speed == 3 || speed >= 4) {
            interval = e.interval ? e.interval - 1u : 0; // already 2^(n-1) x 125 us
        } else {
            u32 frames = e.interval ? e.interval : 1;     // ms -> exponent of 125 us units
            interval = 3;
            while ((1u << interval) < frames * 8 && interval < 10)
                interval++;
        }
    }
    auto* ctx = reinterpret_cast<u32*>(in_ctx(dci + 1));
    ctx[0] = interval << 16;
    ctx[1] = (3u << 1) | (ep_type << 3) | (static_cast<u32>(e.max_packet) << 16);
    ctx[2] = static_cast<u32>(m_eps[dci].ring.phys) | 1;
    ctx[3] = static_cast<u32>(m_eps[dci].ring.phys >> 32);
    ctx[4] = (e.type() == 2 ? 1024u : 8u) | (static_cast<u32>(e.max_packet) << 16);
    return m_hc->command(m_in_phys, 0, (TRB_CONFIGURE_EP << 10) | (static_cast<u32>(m_slot) << 24), nullptr);
}

long UsbDevice::transfer(u8 ep_address, void* data, u32 length) {
    Active active(this);
    if (!active.ok)
        return -err::NODEV;
    u8 dci = static_cast<u8>((ep_address & 0xF) * 2 + ((ep_address & 0x80) ? 1 : 0));
    EndpointState& ep = m_eps[dci];
    if (!ep.buffer || length > ep.buffer_size)
        return -err::INVAL;
    bool in = ep_address & 0x80;
    if (!in)
        memcpy(ep.buffer, data, length);
    ep.done = false;
    ep.wait_trb = ~0ull;
    ep.requested = length;
    u64 trb = ep.ring.push(ep.buffer_phys, length, (TRB_NORMAL << 10) | (1 << 5) | (1 << 2)); // IOC, ISP
    ep.wait_trb = trb;
    m_hc->doorbells[m_slot] = dci;
    if (long r = wait(ep, 5000); r < 0)
        return r;
    u32 actual = length - ep.residual;
    if (in)
        memcpy(data, ep.buffer, actual);
    return actual;
}

void UsbDevice::queue_interrupt(u8 dci) {
    if (gone)
        return;
    EndpointState& ep = m_eps[dci];
    ep.ring.push(ep.buffer_phys, ep.requested, (TRB_NORMAL << 10) | (1 << 5) | (1 << 2));
    m_hc->doorbells[m_slot] = dci;
}

long UsbDevice::start_interrupt_in(u8 ep_address, u32 length, usb::InterruptCallback cb, void* ctx) {
    u8 dci = static_cast<u8>((ep_address & 0xF) * 2 + 1);
    EndpointState& ep = m_eps[dci];
    if (!ep.buffer)
        return -err::INVAL;
    ep.requested = min(length, ep.buffer_size);
    ep.callback_ctx = ctx;
    ep.callback = cb;
    queue_interrupt(dci);
    return 0;
}

void UsbDevice::disable() {
    if (gone)
        return;
    gone = true;
    // Fail every waiting request right away instead of letting it time out.
    for (EndpointState& ep : m_eps) {
        if (!ep.ring.trbs)
            continue;
        ep.callback = nullptr;
        ep.code = CC_DEVICE_GONE;
        ep.done = true;
    }
    m_hc->completions.wake_all();
    // No more events for this slot; wait until the event thread can't be in
    // one of our callbacks any more.
    if (m_hc->slots[m_slot] == this)
        m_hc->slots[m_slot] = nullptr;
    m_hc->wait_event_pass();
    // Requests that were already running return before we free their buffers.
    while (__atomic_load_n(&m_active, __ATOMIC_ACQUIRE))
        sched::sleep_ms(1);
    // The controller stops the endpoints and forgets the slot.
    m_hc->command(0, 0, (TRB_DISABLE_SLOT << 10) | (static_cast<u32>(m_slot) << 24), nullptr);
    m_hc->dcbaa[m_slot] = 0;
    for (EndpointState& ep : m_eps) {
        ep.ring.free();
        if (ep.buffer_phys)
            pmm::free_pages(ep.buffer_phys, ep.buffer_order);
        ep.buffer_phys = 0;
        ep.buffer = nullptr;
        ep.buffer_size = 0;
    }
    if (m_in_phys)
        pmm::free_page(m_in_phys);
    if (m_out_phys)
        pmm::free_page(m_out_phys);
    m_in_phys = m_out_phys = 0;
    m_in = m_out = nullptr;
    // The UsbDevice object stays allocated: class drivers may still hold it.
}

// ---- driver ----

void event_thread(void* arg) {
    auto* hc = static_cast<Controller*>(arg);
    for (;;) {
        hc->process_events();
        __atomic_add_fetch(&hc->event_passes, 1, __ATOMIC_RELEASE);
        sched::sleep_ms(1);
    }
}

Controller* g_controllers = nullptr;

bool match(const dev::Device* d) { return d->bus == dev::Bus::Pci && d->class_code == 0x0C0330; }

long probe(dev::Device* d) {
    pci::Function* f = pci::function_of(d);
    pci::enable(*f);
    auto* hc = new Controller;
    hc->entry = d;
    hc->cap = static_cast<volatile u8*>(pci::map_bar(*f, 0));
    if (!hc->cap || !hc->init())
        return -err::IO;
    u16 version = static_cast<u16>(*reinterpret_cast<volatile u32*>(hc->cap) >> 16); // HCIVERSION (BCD)
    snprintf(d->name, sizeof(d->name), "USB 3 host controller (xHCI %x.%x, %u ports)", version >> 8, (version >> 4) & 0xF,
             hc->max_ports);
    sched::spawn("xhci-events", event_thread, hc, -1, true);
    // Ports are scanned later by enumerate_all().
    Controller** link = &g_controllers;
    u32 n = 1;
    while (*link) {
        link = &(*link)->next;
        n++;
    }
    hc->bus_number = n;
    *link = hc;
    return 0;
}

dev::Driver g_driver{"xhci_hcd", match, probe, {}};

} // namespace

void register_driver() {
    // The USB input layer's decoders are checked first (no hardware needed).
    usb::hid::run_selftest();
    usb::xpad::run_selftest();
    dev::register_driver(&g_driver);
}

void enumerate_all() {
    if (!g_controllers)
        return;
    sched::sleep_ms(50); // let ports settle after power-up
    {
        LockGuard guard(usb::topology_lock());
        for (Controller* hc = g_controllers; hc; hc = hc->next)
            hc->scan_ports();
    }
    usb::start_hotplug(); // from now on, port changes are handled as they come
}

} // namespace xhci
