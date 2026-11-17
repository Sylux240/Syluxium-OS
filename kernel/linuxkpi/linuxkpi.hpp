#pragma once

// LinuxKPI: lets Linux kernel drivers (written in C against Linux's
// internal API) build and run inside Syluxium. Headers live in
// kernel/linuxkpi/include/linux/, implementations in kernel/linuxkpi/*.cpp,
// ported/own Linux-style drivers in kernel/linuxkpi/drivers/ (C files).
namespace linuxkpi {

// Start the timer and workqueue threads and run every driver's
// module_init(). Call before dev::probe_all() so PCI drivers can bind.
void init();

// Internal: PCI/IRQ glue setup (called by init()).
void init_pci();

} // namespace linuxkpi
