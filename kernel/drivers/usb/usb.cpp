#include <linuxkpi/linuxkpi.hpp>
#include <core/log.hpp>
#include <drivers/usb/usb.hpp>
#include <lib/format.hpp>
#include <lib/spinlock.hpp>
#include <lib/string.hpp>
#include <sched/sched.hpp>

namespace usb {

namespace {

void read_string(Device* d, u8 index, char* out, usize size) {
    out[0] = 0;
    if (!index)
        return;
    u8 buf[255];
    long n = d->control(0x80, REQ_GET_DESCRIPTOR, static_cast<u16>((DESC_STRING << 8) | index), 0x0409, buf, sizeof(buf));
    if (n < 2)
        return;
    usize len = 0;
    for (long i = 2; i + 1 < n && i < buf[0] && len + 1 < size; i += 2) {
        u16 c = static_cast<u16>(buf[i] | (buf[i + 1] << 8));
        out[len++] = c < 0x80 ? static_cast<char>(c) : '?';
    }
    out[len] = 0;
}

dev::Category category_for(u8 cls) {
    switch (cls) {
    case 0x01: return dev::Category::Audio;
    case 0x03: return dev::Category::Input;
    case 0x08: return dev::Category::Storage;
    case 0x09: return dev::Category::Usb;      // hub
    case 0x0E: return dev::Category::Multimedia; // video
    case 0xE0: return dev::Category::Network;  // wireless (Bluetooth)
    default:   return dev::Category::Other;
    }
}

// ---- usb-hub worker ----

struct Work {
    void (*fn)(void*);
    void* arg;
    Work* next;
};

IrqSpinlock g_work_lock;
Work* g_work_head = nullptr;
Work* g_work_tail = nullptr;
sched::WaitQueue g_work_wait;
sched::Mutex g_topology_lock;
bool g_worker_started = false;

void worker_thread(void*) {
    for (;;) {
        g_work_wait.wait_until([] { return __atomic_load_n(&g_work_head, __ATOMIC_ACQUIRE) != nullptr; });
        Work* w;
        {
            LockGuard guard(g_work_lock);
            w = g_work_head;
            if (!w)
                continue;
            g_work_head = w->next;
            if (!g_work_head)
                g_work_tail = nullptr;
        }
        {
            LockGuard guard(g_topology_lock);
            w->fn(w->arg);
        }
        delete w;
    }
}

} // namespace

sched::Mutex& topology_lock() { return g_topology_lock; }

void queue_work(void (*fn)(void*), void* arg) {
    auto* w = new Work{fn, arg, nullptr};
    {
        LockGuard guard(g_work_lock);
        if (g_work_tail)
            g_work_tail->next = w;
        else
            g_work_head = w;
        g_work_tail = w;
    }
    g_work_wait.wake_all();
}

void start_hotplug() {
    if (!__atomic_exchange_n(&g_worker_started, true, __ATOMIC_ACQ_REL))
        sched::spawn("usb-hub", worker_thread, nullptr, -1, true);
}

void detach(Device* d) {
    if (d->gone)
        return; // already detached (disable() sets it)
    klog::info("USB %s: %s disconnected", d->path, d->product_name);
    d->disable(); // no callbacks or transfers from here on
    for (int i = d->binding_count; i-- > 0;)
        d->bindings[i].disconnect(d->bindings[i].ctx);
    d->binding_count = 0;
    if (d->entry) {
        dev::remove(d->entry);
        d->entry = nullptr;
    }
}

void attach(Device* d) {
    u8 desc[18];
    if (d->control(0x80, REQ_GET_DESCRIPTOR, DESC_DEVICE << 8, 0, desc, sizeof(desc)) < 18) {
        klog::warn("usb: device did not return its descriptor");
        return;
    }
    d->vendor = static_cast<u16>(desc[8] | (desc[9] << 8));
    d->product = static_cast<u16>(desc[10] | (desc[11] << 8));
    read_string(d, desc[15], d->product_name, sizeof(d->product_name));
    if (!d->product_name[0])
        snprintf(d->product_name, sizeof(d->product_name), "USB device %04x:%04x", d->vendor, d->product);

    // Configuration descriptor: header first, then the whole thing. With
    // several configurations, prefer one with a CDC Ethernet interface
    // (QEMU's usb-net, Realtek RTL815x dongles offer RNDIS/vendor + CDC).
    u8 configs = desc[17] ? desc[17] : 1;
    u8 chosen = 0;
    u8* config = d->config_desc;
    u16 total = 0;
    for (u8 index = 0; index < configs && index < 4; index++) {
        u8 header[9];
        if (d->control(0x80, REQ_GET_DESCRIPTOR, static_cast<u16>((DESC_CONFIGURATION << 8) | index), 0, header, 9) < 9)
            break;
        u16 len = static_cast<u16>(header[2] | (header[3] << 8));
        if (len > sizeof(d->config_desc))
            len = sizeof(d->config_desc);
        if (d->control(0x80, REQ_GET_DESCRIPTOR, static_cast<u16>((DESC_CONFIGURATION << 8) | index), 0, config, len) < len)
            break;
        bool cdc = false;
        for (u16 off = 0; off + 9 <= len && config[off] >= 2; off = static_cast<u16>(off + config[off]))
            if (config[off + 1] == DESC_INTERFACE && config[off + 5] == 2 && (config[off + 6] == 6 || config[off + 6] == 0x0D))
                cdc = true;
        if (index == 0 || cdc) {
            chosen = index;
            total = len;
        }
        if (cdc)
            break;
    }
    if (!total)
        return;
    if (chosen != 0 || configs > 1) { // re-read the chosen one (the loop may have read past it)
        if (d->control(0x80, REQ_GET_DESCRIPTOR, static_cast<u16>((DESC_CONFIGURATION << 8) | chosen), 0, config, total) < total)
            return;
    }
    d->config_len = total;
    if (d->control(0x00, REQ_SET_CONFIGURATION, config[5], 0, nullptr, 0) < 0)
        return;

    // Registry entry for the device itself.
    auto* entry = new dev::Device;
    entry->bus = dev::Bus::Usb;
    entry->parent = d->controller;
    snprintf(entry->bus_id, sizeof(entry->bus_id), "usb%s", d->path);
    snprintf(entry->name, sizeof(entry->name), "%s", d->product_name);
    entry->vendor = d->vendor;
    entry->device = d->product;
    d->entry = entry;

    // Walk interfaces + endpoints and offer each interface to the class drivers.
    Interface intf{};
    bool have_intf = false;
    bool bound = false;
    auto offer = [&]() {
        if (!have_intf || intf.alt_setting != 0)
            return;
        // Every interface gets offered (composite devices: Apple keyboard +
        // trackpad, keyboards with a media-key interface); the device list
        // shows the first driver that bound.
        const char* name = nullptr;
        dev::Category category = category_for(intf.cls);
        const char* linux_driver = nullptr;
        if (linuxkpi::usb_probe(d, intf, &linux_driver)) {
            // A Linux driver (LinuxKPI) whose id_table matches: Wi-Fi dongles.
            category = dev::Category::Network;
            name = linux_driver;
        } else if (bcm5974::probe(d, intf)) {
            category = dev::Category::Input;
            name = "bcm5974";
        } else if (xpad::probe(d, intf)) {
            category = dev::Category::Input;
            name = "xpad";
        } else if (hid::probe(d, intf)) {
            name = "usbhid";
        } else if (storage::probe(d, intf)) {
            name = "usb-storage";
        } else if (hub::probe(d, intf)) {
            name = "hub";
        } else if (cdc_ether::probe(d, intf)) {
            category = dev::Category::Network;
            name = "cdc_ether";
        } else if (audio::probe(d, intf)) {
            category = dev::Category::Audio;
            name = "snd-usb-audio";
        }
        if (name && !bound) {
            entry->category = category;
            entry->class_code = (static_cast<u32>(intf.cls) << 16) | (static_cast<u32>(intf.subclass) << 8) | intf.protocol;
            entry->driver_name = name;
            bound = true;
        }
    };
    for (u16 off = 0; off + 2 <= total && config[off] >= 2;) {
        u8 len = config[off], type = config[off + 1];
        if (type == DESC_INTERFACE && len >= 9) {
            offer();
            intf = Interface{config[off + 2], config[off + 3], config[off + 5], config[off + 6], config[off + 7], {}, 0, 0};
            have_intf = true;
        } else if (type == DESC_HID && len >= 9 && have_intf) {
            // HID class descriptor: (type, length) pairs from byte 6; we want the report descriptor's.
            for (u16 k = 6; k + 2 < len; k += 3)
                if (config[off + k] == DESC_HID_REPORT)
                    intf.hid_report_length = static_cast<u16>(config[off + k + 1] | (config[off + k + 2] << 8));
        } else if (type == DESC_ENDPOINT && len >= 7 && have_intf && intf.endpoint_count < 8) {
            Endpoint& e = intf.endpoints[intf.endpoint_count++];
            e.address = config[off + 2];
            e.attributes = config[off + 3];
            e.max_packet = static_cast<u16>((config[off + 4] | (config[off + 5] << 8)) & 0x7FF);
            e.interval = config[off + 6];
        }
        off = static_cast<u16>(off + len);
    }
    entry->status = dev::Status::NoDriver;
    dev::add(entry);
    klog::ok("USB %s: %s (%04x:%04x)", d->path, d->product_name, d->vendor, d->product);
    offer(); // last interface
    if (bound)
        entry->status = dev::Status::Ok;
}

} // namespace usb
