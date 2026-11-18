// EHCI (USB 2.0) controllers: for now only taken over from the firmware.
//
// While the firmware owns an EHCI controller it may emulate a PS/2 keyboard
// for USB keyboards through SMM traps on ports 0x60/0x64, which can fight
// with the real i8042 driver. The "USB legacy support" capability hands the
// controller to the OS; we take it and stop the SMIs. Devices behind EHCI
// (USB 2 ports on pre-2012 PCs) are not driven yet.

#include <core/log.hpp>
#include <dev/device.hpp>
#include <drivers/pci/pci.hpp>
#include <drivers/usb/usb.hpp>
#include <lib/errno.hpp>
#include <lib/format.hpp>
#include <time/clock.hpp>

namespace ehci {

namespace {

bool match(const dev::Device* d) { return d->bus == dev::Bus::Pci && d->class_code == 0x0C0320; }

long probe(dev::Device* d) {
    pci::Function* f = pci::function_of(d);
    pci::enable(*f);
    auto* cap = static_cast<volatile u8*>(pci::map_bar(*f, 0));
    if (!cap)
        return -err::IO;
    u32 hccparams = *reinterpret_cast<volatile u32*>(cap + 0x08);
    u8 eecp = static_cast<u8>(hccparams >> 8);
    bool handed = false;
    for (int guard = 0; eecp >= 0x40 && guard < 16; guard++) {
        u32 v = pci::read32(*f, eecp);
        if ((v & 0xFF) == 1) { // USBLEGSUP
            pci::write32(*f, eecp, v | (1u << 24)); // OS owned semaphore
            u64 deadline = clock::now_ns() + 1'000'000'000;
            while ((pci::read32(*f, eecp) & (1u << 16)) && clock::now_ns() < deadline) {}
            if (pci::read32(*f, eecp) & (1u << 16)) // the BIOS didn't let go: take it anyway
                pci::write32(*f, eecp, (pci::read32(*f, eecp) & ~(1u << 16)) | (1u << 24));
            pci::write32(*f, eecp + 4, 0);          // USBLEGCTLSTS: no SMIs
            handed = true;
        }
        eecp = static_cast<u8>(v >> 8);
    }
    // Stop the controller (the firmware may have left it running schedules).
    u8 caplen = cap[0];
    auto* usbcmd = reinterpret_cast<volatile u32*>(cap + caplen);
    *usbcmd = *usbcmd & ~1u;
    auto* configflag = reinterpret_cast<volatile u32*>(cap + caplen + 0x40);
    *configflag = 0; // ports back to companion controllers (if any)
    snprintf(d->name, sizeof(d->name), "USB 2 host controller (EHCI%s)", handed ? ", taken from firmware" : "");
    snprintf(d->status_text, sizeof(d->status_text), "USB 2 devices on this controller are not supported yet");
    return 0;
}

dev::Driver g_driver{"ehci_hcd", match, probe, {}};

} // namespace

void register_driver() { dev::register_driver(&g_driver); }

} // namespace ehci
