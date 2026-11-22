#pragma once

// /proc: live view of processes and the system, in Linux's formats so
// ps, top, free, uptime, glibc and future Syluxium apps (Task Manager,
// System page) can read it. /sys: system topology (CPUs today, every device
// once the phase 5 driver model exists).
namespace procfs {

void init(); // mount /proc (needs vfs + processes)

} // namespace procfs

namespace sysfs {

void init(); // build /sys (call once all CPUs are online)
void populate_devices(); // /sys/bus/pci + /sys/kernel/syluxium/devices (after drivers probed)

// A character device node (/sys/dev/char/<major>:<minor>) belonging to a PCI device.
void add_char_device(u32 major, u32 minor, const char* devname, const char* pci_bus_id);
void add_char_device(u32 major, u32 minor, const char* devname, const char* parent_name, bool pci);
// The node went away (its driver unbound): its /sys/dev/char, /sys/class and
// <device>/drm entries go too.
void remove_char_device(u32 major, u32 minor);

// Is this inode /sys or a directory below it? statfs then reports sysfs
// (libudev, and so desktop hardware libraries, only accept device
// directories on sysfs).
bool contains(const void* inode);
} // namespace sysfs
