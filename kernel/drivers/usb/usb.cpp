#include <core/log.hpp>
#include <drivers/usb/usb.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>

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

} // namespace

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

    // Configuration descriptor: header first, then the whole thing.
    u8 header[9];
    if (d->control(0x80, REQ_GET_DESCRIPTOR, DESC_CONFIGURATION << 8, 0, header, 9) < 9)
        return;
    u16 total = static_cast<u16>(header[2] | (header[3] << 8));
    if (total > 1024)
        total = 1024;
    u8 config[1024];
    if (d->control(0x80, REQ_GET_DESCRIPTOR, DESC_CONFIGURATION << 8, 0, config, total) < total)
        return;
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
        entry->category = category_for(intf.cls);
        entry->class_code = (static_cast<u32>(intf.cls) << 16) | (static_cast<u32>(intf.subclass) << 8) | intf.protocol;
        if (hid::probe(d, intf)) {
            entry->driver_name = "usbhid";
            bound = true;
        } else if (storage::probe(d, intf)) {
            entry->driver_name = "usb-storage";
            bound = true;
        } else if (hub::probe(d, intf)) {
            entry->driver_name = "hub";
            bound = true;
        }
    };
    for (u16 off = 0; off + 2 <= total && config[off] >= 2;) {
        u8 len = config[off], type = config[off + 1];
        if (type == DESC_INTERFACE && len >= 9) {
            offer();
            intf = Interface{config[off + 2], config[off + 3], config[off + 5], config[off + 6], config[off + 7], {}, 0};
            have_intf = true;
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
