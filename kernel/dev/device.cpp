#include <core/log.hpp>
#include <dev/device.hpp>
#include <lib/format.hpp>
#include <lib/spinlock.hpp>

namespace dev {

namespace {

IntrusiveList<Device, &Device::node> g_devices;
IntrusiveList<Driver, &Driver::node> g_drivers;
IrqSpinlock g_lock;
u32 g_next_id = 1;

} // namespace

Device* add(Device* d) {
    LockGuard guard(g_lock);
    d->id = g_next_id++;
    g_devices.push_back(d);
    return d;
}

void register_driver(Driver* drv) {
    LockGuard guard(g_lock);
    g_drivers.push_back(drv);
}

void probe_all() {
    // Probing may sleep and allocate, so walk the lists without the lock
    // (devices and drivers are only ever added, never removed, for now).
    for (Device* d = g_devices.front(); d; d = g_devices.next_of(d)) {
        if (d->driver_name)
            continue;
        for (Driver* drv = g_drivers.front(); drv; drv = g_drivers.next_of(drv)) {
            if (drv->match_ex ? !drv->match_ex(drv, d) : !drv->match(d))
                continue;
            d->driver_name = drv->name;
            long r = drv->probe_ex ? drv->probe_ex(drv, d) : drv->probe(d);
            if (r == 0) {
                d->status = Status::Ok;
                break;
            }
            d->status = Status::Error;
            snprintf(d->status_text, sizeof(d->status_text), "%s failed to start (error %ld)", drv->name, -r);
            klog::warn("%s %s: %s", d->bus_id, d->name, d->status_text);
            break;
        }
    }
}

void for_each(void (*fn)(Device*, void*), void* ctx) {
    for (Device* d = g_devices.front(); d; d = g_devices.next_of(d))
        fn(d, ctx);
}

u32 count() {
    LockGuard guard(g_lock);
    return static_cast<u32>(g_devices.size());
}

const char* category_name(Category c) {
    switch (c) {
    case Category::Processor:  return "Processors";
    case Category::System:     return "System devices";
    case Category::Bridge:     return "Bridges";
    case Category::Storage:    return "Storage controllers";
    case Category::Network:    return "Network adapters";
    case Category::Display:    return "Display adapters";
    case Category::Audio:      return "Sound";
    case Category::Input:      return "Keyboards and mice";
    case Category::Usb:        return "USB controllers";
    case Category::Serial:     return "Ports (COM)";
    case Category::Multimedia: return "Multimedia";
    default:                   return "Other devices";
    }
}

const char* bus_name(Bus b) {
    switch (b) {
    case Bus::Acpi: return "acpi";
    case Bus::Pci:  return "pci";
    case Bus::Usb:  return "usb";
    case Bus::Cpu:  return "cpu";
    default:        return "platform";
    }
}

} // namespace dev
