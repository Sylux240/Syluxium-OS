// /sys views of the device registry:
//   /sys/bus/pci/devices/<addr>/...   Linux-compatible (lspci, udev-style tools)
//   /sys/kernel/syluxium/devices      every device, one tab-separated line each
//                                     (what the Syluxium Device Manager reads)

#include <dev/device.hpp>
#include <drivers/pci/pci.hpp>
#include <fs/genfs.hpp>
#include <fs/procfs.hpp>
#include <fs/vfs.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>

namespace sysfs {

namespace {

using genfs::GeneratedFile;
using genfs::GeneratedLink;
using genfs::TextBuffer;

bool gen_vendor(TextBuffer& o, const void* c) { o.printf("0x%04x\n", static_cast<const dev::Device*>(c)->vendor); return true; }
bool gen_device(TextBuffer& o, const void* c) { o.printf("0x%04x\n", static_cast<const dev::Device*>(c)->device); return true; }
bool gen_subvendor(TextBuffer& o, const void* c) { o.printf("0x%04x\n", static_cast<const dev::Device*>(c)->subsystem_vendor); return true; }
bool gen_subdevice(TextBuffer& o, const void* c) { o.printf("0x%04x\n", static_cast<const dev::Device*>(c)->subsystem_device); return true; }
bool gen_class(TextBuffer& o, const void* c) { o.printf("0x%06x\n", static_cast<const dev::Device*>(c)->class_code); return true; }
bool gen_revision(TextBuffer& o, const void* c) { o.printf("0x%02x\n", static_cast<const dev::Device*>(c)->revision); return true; }

bool gen_irq(TextBuffer& o, const void* c) {
    auto* d = static_cast<const dev::Device*>(c);
    pci::Function* f = pci::function_of(d);
    o.printf("%u\n", f ? f->irq_line : 0);
    return true;
}

// Linux "resource" file: start end flags, one line per BAR (+ ROM and bridge windows).
bool gen_resource(TextBuffer& o, const void* c) {
    pci::Function* f = pci::function_of(static_cast<const dev::Device*>(c));
    for (int i = 0; i < 13; i++) {
        if (f && i < 6 && f->bars[i].size) {
            const pci::Bar& b = f->bars[i];
            u64 flags = b.is_io ? 0x101 : (0x200 | (b.prefetchable ? 0x2000 : 0) | (b.is_64bit ? 0x100000 : 0));
            o.printf("0x%016lx 0x%016lx 0x%016lx\n", b.address, b.address + b.size - 1, flags);
        } else {
            o.append("0x0000000000000000 0x0000000000000000 0x0000000000000000\n");
        }
    }
    return true;
}

// Raw configuration space (binary), as lspci -x / -v reads it.
bool gen_config(TextBuffer& o, const void* c) {
    pci::Function* f = pci::function_of(static_cast<const dev::Device*>(c));
    if (!f)
        return false;
    u32 size = f->pcie_cap ? 4096 : 256;
    for (u32 off = 0; off < size; off += 4) {
        u32 v = pci::read32(*f, static_cast<u16>(off));
        o.append(reinterpret_cast<const char*>(&v), 4);
    }
    return true;
}

bool link_driver(char* out, usize size, const void* c) {
    auto* d = static_cast<const dev::Device*>(c);
    if (!d->driver_name)
        return false;
    snprintf(out, size, "../../drivers/%s", d->driver_name);
    return true;
}

const char* status_name(dev::Status s) {
    switch (s) {
    case dev::Status::Ok:    return "ok";
    case dev::Status::Error: return "error";
    default:                 return "no-driver";
    }
}

// id  bus  bus-id  category  vendor:device  status  driver  name  [status text]
bool gen_device_list(TextBuffer& o, const void*) {
    dev::for_each([](dev::Device* d, void* arg) {
        auto* out = static_cast<TextBuffer*>(arg);
        out->printf("%u\t%s\t%s\t%s\t%04x:%04x\t%s\t%s\t%s\t%s\n", d->id, dev::bus_name(d->bus), d->bus_id,
                    dev::category_name(d->category), d->vendor, d->device, status_name(d->status),
                    d->driver_name ? d->driver_name : "-", d->name, d->status_text);
    }, &o);
    return true;
}

void add(const char* dir, const char* name, vfs::Inode* node) {
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    vfs::add_node(path, node);
}

} // namespace

void populate_devices() {
    vfs::mkdir_p("/sys/bus/pci/devices", 0755);
    vfs::mkdir_p("/sys/bus/pci/drivers", 0755);
    vfs::mkdir_p("/sys/kernel/syluxium", 0755);
    vfs::add_node("/sys/kernel/syluxium/devices", new GeneratedFile(gen_device_list, nullptr));

    dev::for_each([](dev::Device* d, void*) {
        if (d->bus != dev::Bus::Pci)
            return;
        char dir[96];
        snprintf(dir, sizeof(dir), "/sys/bus/pci/devices/%s", d->bus_id);
        vfs::mkdir_p(dir, 0755);
        add(dir, "vendor", new GeneratedFile(gen_vendor, d));
        add(dir, "device", new GeneratedFile(gen_device, d));
        add(dir, "subsystem_vendor", new GeneratedFile(gen_subvendor, d));
        add(dir, "subsystem_device", new GeneratedFile(gen_subdevice, d));
        add(dir, "class", new GeneratedFile(gen_class, d));
        add(dir, "revision", new GeneratedFile(gen_revision, d));
        add(dir, "irq", new GeneratedFile(gen_irq, d));
        add(dir, "resource", new GeneratedFile(gen_resource, d));
        add(dir, "config", new GeneratedFile(gen_config, d, 0644));
        if (d->driver_name) {
            add(dir, "driver", new GeneratedLink(link_driver, d));
            char drv[96];
            snprintf(drv, sizeof(drv), "/sys/bus/pci/drivers/%s", d->driver_name);
            vfs::mkdir_p(drv, 0755);
        }
    }, nullptr);
}

} // namespace sysfs
