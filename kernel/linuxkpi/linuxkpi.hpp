#pragma once

// LinuxKPI: lets Linux kernel drivers (written in C against Linux's
// internal API) build and run inside Syluxium. Headers live in
// kernel/linuxkpi/include/linux/, implementations in kernel/linuxkpi/*.cpp,
// ported/own Linux-style drivers in kernel/linuxkpi/drivers/ (C files).
namespace usb {
class Device;
struct Interface;
}
namespace net {
class Socket;
}

namespace linuxkpi {

// Start the timer and workqueue threads and run every driver's
// module_init(). Call before dev::probe_all() so PCI drivers can bind.
void init();

// The AML interpreter runs now: retry GPU probes that failed while their
// ACPI companion wasn't available yet.
void acpi_ready();

// Internal: PCI/IRQ glue setup (called by init()).
void init_pci();

// USB core hook: offer a device's interface to the Linux USB drivers
// (struct usb_driver id_tables). On a match the driver's probe() runs and
// *driver_name is set. Topology lock held (called from usb::attach).
bool usb_probe(usb::Device* d, const usb::Interface& intf, const char** driver_name);

// socket(2) for an address family a Linux driver registered (AF_BLUETOOTH):
// a socket, or nullptr with *error set (-EAFNOSUPPORT if none did).
net::Socket* create_socket(int family, int type, int protocol, long* error);

} // namespace linuxkpi
