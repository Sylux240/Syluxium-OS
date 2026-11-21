// HID over I2C (see i2c_hid.hpp). Protocol: Microsoft's "HID over I2C"
// specification 1.0, as implemented by Linux's i2c-hid; our own code.

#include <acpi/devices.hpp>
#include <core/log.hpp>
#include <drivers/i2c/designware.hpp>
#include <drivers/i2c/i2c_hid.hpp>
#include <drivers/pci/pci.hpp>
#include <drivers/usb/hid_device.hpp>
#include <lib/errno.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <mm/heap.hpp>
#include <mm/vmalloc.hpp>
#include <sched/sched.hpp>
#include <time/clock.hpp>

#include <uacpi/namespace.h>
#include <uacpi/resources.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

namespace i2c_hid {

namespace {

// ---- controllers ----

struct Controller {
    uacpi_namespace_node* node = nullptr;
    i2c::DesignWare dw;
    bool ok = false;
    char name[24] = {};
};

constexpr int MAX_CONTROLLERS = 8;
Controller g_controllers[MAX_CONTROLLERS];
int g_controller_count = 0;
sched::Mutex g_setup_lock;

u64 package_int(uacpi_object_array& a, usize i) {
    u64 v = 0;
    if (i < a.count)
        uacpi_object_get_integer(a.objects[i], &v);
    return v;
}

// Timing parameters the firmware gives for its board (Windows uses them):
// SSCN (100 kHz) / FMCN (400 kHz) = {hcnt, lcnt, sda hold}.
i2c::Timing acpi_timing(uacpi_namespace_node* node, bool fast) {
    i2c::Timing t;
    uacpi_object* obj = nullptr;
    if (uacpi_eval_simple(node, fast ? "FMCN" : "SSCN", &obj) != UACPI_STATUS_OK || !obj)
        return t;
    uacpi_object_array a;
    if (uacpi_object_get_package(obj, &a) == UACPI_STATUS_OK && a.count >= 2) {
        t.hcnt = static_cast<u16>(package_int(a, 0));
        t.lcnt = static_cast<u16>(package_int(a, 1));
        t.sda_hold = static_cast<u16>(package_int(a, 2));
    }
    uacpi_object_unref(obj);
    return t;
}

struct MemSearch {
    u64 base = 0;
    u32 length = 0;
};

uacpi_iteration_decision find_memory(void* user, uacpi_resource* r) {
    auto* m = static_cast<MemSearch*>(user);
    if (r->type == UACPI_RESOURCE_TYPE_FIXED_MEMORY32 && !m->base) {
        m->base = r->fixed_memory32.address;
        m->length = r->fixed_memory32.length;
    } else if (r->type == UACPI_RESOURCE_TYPE_MEMORY32 && !m->base) {
        m->base = r->memory32.minimum;
        m->length = r->memory32.length;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

// Intel LPSS input clocks (Linux's intel-lpss-pci tables): Cannon Lake
// 216 MHz, Elkhart Lake 100 MHz, Broxton/Apollo/Gemini Lake and Ice Lake
// on 133 MHz, Sunrise Point (Skylake/Kaby Lake) 120 MHz.
u32 lpss_clock_khz(u16 device) {
    switch (device & 0xFFF0) {
    case 0x9D60: case 0xA160: case 0xA2E0: return 120000;              // Sunrise Point LP/H, Union Point
    case 0x9DE0: case 0xA360: case 0x02E0: case 0x06E0: return 216000; // Cannon Point LP/H, Comet Lake LP/H
    case 0x4B40: case 0x4B70: return 100000;                           // Elkhart Lake
    default: return 133000;                                            // Broxton .. Ice/Tiger/Alder Lake
    }
}

// The controller ACPI names in a device's I2cSerialBus resource.
Controller* controller_for(const char* path) {
    uacpi_namespace_node* node = nullptr;
    if (!path || uacpi_namespace_node_find(nullptr, path, &node) != UACPI_STATUS_OK || !node) {
        klog::warn("i2c-hid: controller %s not found in ACPI", path ? path : "(none)");
        return nullptr;
    }
    LockGuard guard(g_setup_lock);
    for (int i = 0; i < g_controller_count; i++)
        if (g_controllers[i].node == node)
            return g_controllers[i].ok ? &g_controllers[i] : nullptr;
    if (g_controller_count == MAX_CONTROLLERS)
        return nullptr;
    Controller& c = g_controllers[g_controller_count++];
    c.node = node;
    const char* abs = uacpi_namespace_node_generate_absolute_path(node);
    snprintf(c.name, sizeof(c.name), "%s", abs ? abs : path);
    if (abs)
        uacpi_free_absolute_path(abs);

    uacpi_execute_simple(node, "_PS0"); // power it up (D0), if the firmware has a method for that
    volatile u8* regs = nullptr;
    u32 clock_khz = 133000;
    u64 adr = 0;
    pci::Function* pf = nullptr;
    if (uacpi_eval_adr(node, &adr) == UACPI_STATUS_OK) {
        u64 bbn = 0;
        uacpi_namespace_node* parent = uacpi_namespace_node_parent(node);
        if (parent)
            uacpi_eval_simple_integer(parent, "_BBN", &bbn);
        u8 dev = static_cast<u8>(adr >> 16), fn = static_cast<u8>(adr & 0xFFFF);
        for (u32 i = 0; i < pci::count(); i++) {
            pci::Function* f = pci::function_at(i);
            if (f->segment == 0 && f->bus == bbn && f->device == dev && f->function == fn)
                pf = f;
        }
    }
    if (pf) {
        // PCI power management: D0 (the firmware may have left it in D3).
        for (u8 cap = pci::read8(*pf, 0x34) & 0xFC, n = 0; cap && n < 48; cap = pci::read8(*pf, cap + 1) & 0xFC, n++)
            if (pci::read8(*pf, cap) == 0x01) {
                u16 pmcsr = pci::read16(*pf, cap + 4);
                if (pmcsr & 3) {
                    pci::write16(*pf, cap + 4, pmcsr & ~3);
                    sched::sleep_ms(10);
                }
            }
        pci::enable(*pf);
        regs = static_cast<volatile u8*>(pci::map_bar(*pf, 0));
        if (regs && pf->vendor_id == 0x8086) {
            // LPSS private registers: take the function out of reset; the
            // DMA engine's remap register gets the BAR address.
            auto priv = [&](u32 off) -> volatile u32& { return *reinterpret_cast<volatile u32*>(regs + 0x200 + off); };
            priv(0x04) = 0;
            priv(0x04) = 0x3 | (1 << 2);
            if (!(priv(0xFC) & (1 << 8))) { // has iDMA
                priv(0x40) = static_cast<u32>(pf->bars[0].address);
                priv(0x44) = static_cast<u32>(pf->bars[0].address >> 32);
            }
            clock_khz = lpss_clock_khz(pf->device_id);
        } else if (pf->vendor_id == 0x1022) {
            clock_khz = 150000;
        }
    } else {
        MemSearch m;
        uacpi_for_each_device_resource(node, "_CRS", find_memory, &m);
        if (m.base)
            regs = static_cast<volatile u8*>(vmalloc::map_physical(m.base, m.length >= 0x1000 ? m.length : 0x1000));
        uacpi_id_string* hid = nullptr;
        if (uacpi_eval_hid(node, &hid) == UACPI_STATUS_OK && hid) {
            if (!strncmp(hid->value, "AMD", 3))
                clock_khz = 150000; // AMD FCH I2C (AMDI0010/0019/0510)
            else if (!strncmp(hid->value, "INT33C", 6) || !strncmp(hid->value, "INT343", 6))
                clock_khz = 100000; // Haswell/Broadwell LPSS
            uacpi_free_id_string(hid);
        }
    }
    if (!regs) {
        klog::warn("i2c-hid: %s: controller registers not found (%s)", c.name, pf ? "PCI BAR" : "ACPI _CRS");
        return nullptr;
    }
    // 400 kHz (every touchpad supports it); the ACPI timing for that speed.
    i2c::Timing t = acpi_timing(node, true);
    c.ok = c.dw.init(regs, clock_khz, 400000, t, c.name);
    return c.ok ? &c : nullptr;
}

// ---- devices ----

constexpr u8 OP_RESET = 1, OP_GET_REPORT = 2, OP_SET_REPORT = 3, OP_SET_POWER = 8;

struct HidDescriptor {
    u16 length, version, report_desc_length, report_desc_register, input_register, max_input_length,
        output_register, max_output_length, command_register, data_register, vendor, product, version_id;
};

class I2cHid final : public usb::hid::FeatureIo {
public:
    Controller* bus = nullptr;
    u16 addr = 0, desc_register = 1;
    char acpi_hid[16] = {};
    char name[40] = {};
    HidDescriptor hd = {};
    usb::hid::HidDevice dev;

    long read_register(u16 reg, u8* buf, u32 len) {
        u8 w[2] = {static_cast<u8>(reg), static_cast<u8>(reg >> 8)};
        return bus->dw.transfer(addr, w, 2, buf, len);
    }

    // Command register + opcode word (+ report ID byte when >= 15).
    u32 encode_command(u8* out, u8 opcode, u8 type, u8 id) {
        u32 n = 0;
        out[n++] = static_cast<u8>(hd.command_register);
        out[n++] = static_cast<u8>(hd.command_register >> 8);
        if (id < 0x0F) {
            out[n++] = static_cast<u8>((type << 4) | id);
            out[n++] = opcode;
        } else {
            out[n++] = static_cast<u8>((type << 4) | 0x0F);
            out[n++] = opcode;
            out[n++] = id;
        }
        return n;
    }

    long simple_command(u8 opcode, u8 low) {
        u8 w[4] = {static_cast<u8>(hd.command_register), static_cast<u8>(hd.command_register >> 8), low, opcode};
        return bus->dw.transfer(addr, w, 4, nullptr, 0);
    }

    long get_feature(u8 id, u8* buf, u32 len) override {
        u8 w[16];
        u32 n = encode_command(w, OP_GET_REPORT, 3, id);
        w[n++] = static_cast<u8>(hd.data_register);
        w[n++] = static_cast<u8>(hd.data_register >> 8);
        u8 r[66];
        u32 want = len + 2 > sizeof(r) ? sizeof(r) : len + 2;
        long e = bus->dw.transfer(addr, w, n, r, want);
        if (e < 0)
            return e;
        u32 got = r[0] | (r[1] << 8);
        if (got <= 2)
            return 0;
        got = (got > want ? want : got) - 2;
        memcpy(buf, r + 2, got);
        return got;
    }

    long set_feature(u8 id, const u8* buf, u32 len) override {
        u8 w[96];
        u32 n = encode_command(w, OP_SET_REPORT, 3, id);
        w[n++] = static_cast<u8>(hd.data_register);
        w[n++] = static_cast<u8>(hd.data_register >> 8);
        // The data register gets: length (2 bytes, itself included), report ID, data.
        const u8* data = buf;
        u32 dlen = len;
        if (id && dlen) { // the caller's buffer starts with the ID: it goes first anyway
            data++;
            dlen--;
        }
        u32 size = 2 + (id ? 1 : 0) + dlen;
        if (n + size > sizeof(w))
            return -err::INVAL;
        w[n++] = static_cast<u8>(size);
        w[n++] = static_cast<u8>(size >> 8);
        if (id)
            w[n++] = id;
        memcpy(w + n, data, dlen);
        n += dlen;
        long e = bus->dw.transfer(addr, w, n, nullptr, 0);
        return e < 0 ? e : static_cast<long>(len);
    }

    long read_input(u8* buf, u32 len) { return bus->dw.transfer(addr, nullptr, 0, buf, len); }
};

u16 le16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }

bool fetch_descriptor(I2cHid* d) {
    u8 raw[30];
    long e = -1;
    for (int attempt = 0; attempt < 3; attempt++) { // some need time after power-up
        e = d->read_register(d->desc_register, raw, sizeof(raw));
        if (e == 0)
            break;
        sched::sleep_ms(100);
    }
    if (e < 0) {
        klog::info("i2c-hid: %s at %#x: no answer (%ld)%s", d->acpi_hid, d->addr, e,
                   e == -err::NXIO ? " - not fitted on this model" : "");
        return false;
    }
    HidDescriptor& h = d->hd;
    u16* fields = &h.length;
    for (int i = 0; i < 13; i++)
        fields[i] = le16(raw + 2 * i);
    if (h.length != 30 || h.version != 0x0100 || !h.report_desc_length || h.report_desc_length > 4096 ||
        h.max_input_length < 2 || h.max_input_length > 1024) {
        klog::warn("i2c-hid: %s at %#x: unexpected HID descriptor (length %u, version %#x, report descriptor %u, "
                   "max input %u)",
                   d->acpi_hid, d->addr, h.length, h.version, h.report_desc_length, h.max_input_length);
        return false;
    }
    return true;
}

// Reset: the device answers with a zero-length input report when done.
void reset_device(I2cHid* d, u8* buf) {
    d->simple_command(OP_SET_POWER, 0); // power on
    sched::sleep_ms(2);
    if (d->simple_command(OP_RESET, 0) < 0) {
        klog::warn("i2c-hid: %s: reset command failed", d->name);
        return;
    }
    u64 deadline = clock::now_ns() + 1'000'000'000;
    while (clock::now_ns() < deadline) {
        sched::sleep_ms(10);
        if (d->read_input(buf, d->hd.max_input_length) == 0 && le16(buf) == 0)
            return;
    }
    klog::info("i2c-hid: %s: no reset acknowledgement (going on)", d->name);
}

void device_thread(void* arg) {
    auto* d = static_cast<I2cHid*>(arg);
    if (!fetch_descriptor(d))
        return; // the object stays (tiny); nothing else refers to it
    snprintf(d->name, sizeof(d->name), "%s:00 %04X:%04X", d->acpi_hid, d->hd.vendor, d->hd.product);
    u32 max = d->hd.max_input_length;
    auto* buf = static_cast<u8*>(kmalloc(max));
    auto* last = static_cast<u8*>(kmalloc(max));
    auto* desc = static_cast<u8*>(kmalloc(d->hd.report_desc_length));
    if (!buf || !last || !desc) {
        klog::warn("i2c-hid: %s: out of memory", d->name);
        return;
    }
    reset_device(d, buf);
    long e = d->read_register(d->hd.report_desc_register, desc, d->hd.report_desc_length);
    if (e < 0) {
        klog::warn("i2c-hid: %s: reading the report descriptor failed (%ld)", d->name, e);
        return;
    }
    int usable = d->dev.parse(desc, d->hd.report_desc_length);
    if (!usable) {
        klog::warn("i2c-hid: %s: report descriptor not usable (%u bytes, starts %02x %02x %02x %02x)", d->name,
                   d->hd.report_desc_length, desc[0], desc[1], desc[2], desc[3]);
        return;
    }
    int n = d->dev.register_devices(d->name, "", 0x18 /* BUS_I2C */, d->hd.vendor, d->hd.product, d);
    klog::ok("i2c-hid: %s on %s address %#x: %d input device(s) (report descriptor %u bytes, polled)", d->name,
             d->bus->name, d->addr, n, d->hd.report_desc_length);

    // Poll the input register (no GPIO interrupt driver yet). A device with
    // nothing to say answers with length 0; repeats of the last report are
    // dropped (some answer with it again).
    u32 last_len = 0, errors = 0;
    for (;;) {
        sched::sleep_ms(8);
        e = d->read_input(buf, max);
        if (e < 0) {
            if (++errors == 50)
                klog::warn("i2c-hid: %s: input reads keep failing (%ld); slowing down", d->name, e);
            if (errors >= 50)
                sched::sleep_ms(1000);
            continue;
        }
        errors = 0;
        u32 len = le16(buf);
        if (len <= 2 || len > max)
            continue; // nothing pending (0), or not ready (0xFFFF)
        if (len == last_len && memcmp(buf, last, len) == 0)
            continue;
        memcpy(last, buf, len);
        last_len = len;
        d->dev.report(buf + 2, len - 2);
    }
}

// ---- ACPI enumeration ----

struct CrsInfo {
    char controller[64] = {};
    u16 addr = 0;
    u32 speed = 0;
    bool ten_bit = false;
    const char* irq = "none";
};

uacpi_iteration_decision parse_crs(void* user, uacpi_resource* r) {
    auto* c = static_cast<CrsInfo*>(user);
    if (r->type == UACPI_RESOURCE_TYPE_SERIAL_I2C_CONNECTION && !c->controller[0]) {
        const uacpi_resource_i2c_connection& i = r->i2c_connection;
        c->addr = i.slave_address;
        c->speed = i.connection_speed;
        c->ten_bit = i.addressing_mode == UACPI_I2C_10BIT;
        if (i.common.source.string)
            snprintf(c->controller, sizeof(c->controller), "%s", i.common.source.string);
    } else if (r->type == UACPI_RESOURCE_TYPE_GPIO_CONNECTION) {
        c->irq = "GPIO";
    } else if (r->type == UACPI_RESOURCE_TYPE_EXTENDED_IRQ || r->type == UACPI_RESOURCE_TYPE_IRQ) {
        c->irq = "APIC";
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

// _DSM function 1 of the HID-over-I2C UUID: the HID descriptor's register.
u16 descriptor_register(uacpi_namespace_node* node) {
    static const u8 uuid[16] = {0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45,
                                0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE};
    uacpi_data_view view;
    view.const_bytes = uuid;
    view.length = sizeof(uuid);
    uacpi_object* args[4] = {uacpi_object_create_buffer(view), uacpi_object_create_integer(1),
                             uacpi_object_create_integer(1), uacpi_object_create_package({nullptr, 0})};
    uacpi_object_array arr{args, 4};
    u64 reg = 0;
    bool ok = args[0] && args[1] && args[2] && args[3] &&
              uacpi_eval_integer(node, "_DSM", &arr, &reg) == UACPI_STATUS_OK;
    for (auto* a : args)
        if (a)
            uacpi_object_unref(a);
    return ok ? static_cast<u16>(reg) : 0xFFFF;
}

constexpr int MAX_DEVICES = 8;
uacpi_namespace_node* g_nodes[MAX_DEVICES];
int g_found = 0;

uacpi_iteration_decision collect(void*, uacpi_namespace_node* node, uacpi_u32) {
    for (int i = 0; i < g_found; i++)
        if (g_nodes[i] == node)
            return UACPI_ITERATION_DECISION_CONTINUE; // both IDs
    if (g_found < MAX_DEVICES)
        g_nodes[g_found++] = node;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

void probe_device(uacpi_namespace_node* node) {
    const char* path = uacpi_namespace_node_generate_absolute_path(node);
    CrsInfo crs;
    uacpi_for_each_device_resource(node, "_CRS", parse_crs, &crs);
    u16 reg = descriptor_register(node);
    uacpi_id_string* hid = nullptr;
    uacpi_eval_hid(node, &hid);
    klog::info("i2c-hid: %s (%s): bus %s, address %#x, %u kHz, HID descriptor at %#x, interrupt %s", path ? path : "?",
               hid ? hid->value : "?", crs.controller[0] ? crs.controller : "?", crs.addr, crs.speed / 1000, reg,
               crs.irq);
    if (path)
        uacpi_free_absolute_path(path);
    if (!crs.controller[0] || reg == 0xFFFF || crs.ten_bit || !crs.addr) {
        klog::warn("i2c-hid: incomplete ACPI description (%s), skipped",
                   !crs.controller[0] ? "no I2C bus" : reg == 0xFFFF ? "no _DSM" : "10-bit address");
        if (hid)
            uacpi_free_id_string(hid);
        return;
    }
    uacpi_execute_simple(node, "_PS0");
    auto* d = new I2cHid;
    d->addr = crs.addr;
    d->desc_register = reg;
    snprintf(d->acpi_hid, sizeof(d->acpi_hid), "%s", hid ? hid->value : "PNP0C50");
    if (hid)
        uacpi_free_id_string(hid);
    d->bus = controller_for(crs.controller);
    if (!d->bus) {
        klog::warn("i2c-hid: %s: its I2C controller can't be used", d->acpi_hid);
        delete d;
        return;
    }
    sched::spawn("i2c-hid", device_thread, d, -1, true);
}

void probe_thread(void*) {
    for (int i = 0; i < g_found; i++)
        probe_device(g_nodes[i]);
}

} // namespace

void init() {
    if (!acpi::interpreter_ready())
        return;
    uacpi_find_devices("PNP0C50", collect, nullptr);
    uacpi_find_devices("ACPI0C50", collect, nullptr);
    if (!g_found)
        return;
    klog::info("i2c-hid: %d device(s) in ACPI", g_found);
    sched::spawn("i2c-hid-probe", probe_thread, nullptr, -1, true);
}

} // namespace i2c_hid
