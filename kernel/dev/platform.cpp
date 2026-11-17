// Devices that aren't on an enumerable bus: CPUs, interrupt controllers,
// timers, legacy ports. Registered so the Device Manager shows everything.

#include <acpi/acpi.hpp>
#include <arch/x86_64/apic.hpp>
#include <arch/x86_64/cpu.hpp>
#include <arch/x86_64/percpu.hpp>
#include <dev/device.hpp>
#include <dev/platform.hpp>
#include <drivers/ps2kbd.hpp>
#include <drivers/serial.hpp>
#include <lib/format.hpp>
#include <lib/string.hpp>
#include <video/console.hpp>
#include <video/framebuffer.hpp>

namespace dev {

namespace {

Device* platform(const char* bus_id, const char* name, Category cat, const char* driver) {
    auto* d = new Device;
    d->bus = Bus::Platform;
    snprintf(d->bus_id, sizeof(d->bus_id), "%s", bus_id);
    snprintf(d->name, sizeof(d->name), "%s", name);
    d->category = cat;
    d->driver_name = driver;
    d->status = driver ? Status::Ok : Status::NoDriver;
    return d;
}

void cpu_brand(char* out, usize size) {
    char brand[49] = {};
    if (arch::cpuid(0x80000000).eax >= 0x80000004) {
        for (u32 i = 0; i < 3; i++) {
            auto r = arch::cpuid(0x80000002 + i);
            memcpy(brand + i * 16 + 0, &r.eax, 4);
            memcpy(brand + i * 16 + 4, &r.ebx, 4);
            memcpy(brand + i * 16 + 8, &r.ecx, 4);
            memcpy(brand + i * 16 + 12, &r.edx, 4);
        }
    }
    const char* b = brand;
    while (*b == ' ') b++;
    snprintf(out, size, "%s", *b ? b : "x86-64 processor");
}

} // namespace

void register_platform_devices(bool keyboard) {
    char brand[64];
    cpu_brand(brand, sizeof(brand));
    for (u32 i = 0; i < percpu::count(); i++) {
        Cpu* c = percpu::get(i);
        if (!c || !c->online)
            continue;
        char id[16];
        snprintf(id, sizeof(id), "cpu%u", c->index);
        Device* d = platform(id, brand, Category::Processor, "sched");
        d->bus = Bus::Cpu;
        add(d);
    }

    const auto& madt = acpi::madt();
    add(platform("lapic", apic::x2apic_mode() ? "Local APIC (x2APIC mode)" : "Local APIC", Category::System, "apic"));
    for (u32 i = 0; i < madt.ioapic_count; i++) {
        char id[16];
        snprintf(id, sizeof(id), "ioapic%u", i);
        Device* d = platform(id, "I/O APIC", Category::System, "ioapic");
        d->add_resource(Resource::Mmio, madt.ioapics[i].address, 0x20);
        add(d);
    }
    if (acpi::hpet().present) {
        Device* d = platform("hpet", "High Precision Event Timer", Category::System, "clock");
        d->add_resource(Resource::Mmio, acpi::hpet().address, 1024);
        add(d);
    }
    Device* rtc = platform("rtc", "CMOS real-time clock", Category::System, "rtc");
    rtc->add_resource(Resource::Io, 0x70, 2);
    add(rtc);

    if (serial::present()) {
        Device* d = platform("serial0", "Communications port (COM1)", Category::Serial, "serial");
        d->add_resource(Resource::Io, 0x3F8, 8);
        d->add_resource(Resource::Irq, 4, 1);
        add(d);
    }
    Device* kbd = platform("i8042", "PS/2 keyboard", Category::Input, keyboard ? "ps2kbd" : nullptr);
    kbd->add_resource(Resource::Io, 0x60, 1);
    kbd->add_resource(Resource::Io, 0x64, 1);
    kbd->add_resource(Resource::Irq, 1, 1);
    add(kbd);
    if (keyboard && ps2kbd::mouse_present()) {
        Device* mouse = platform("i8042-aux", "PS/2 mouse (wheel)", Category::Input, "ps2mouse");
        mouse->add_resource(Resource::Irq, 12, 1);
        add(mouse);
    }

    auto& fb = video::primary_framebuffer();
    if (fb.valid()) {
        char name[96];
        snprintf(name, sizeof(name), "Firmware framebuffer (%ux%u)", fb.width(), fb.height());
        add(platform("fb0", name, Category::Display, "fbcon"));
    }
}

} // namespace dev
