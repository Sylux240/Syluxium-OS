#pragma once

#include <lib/types.hpp>

namespace net {

// A device appeared or went away ("add"/"remove"): tell NETLINK_KOBJECT_UEVENT
// listeners. `devpath` is under /sys ("/devices/virtual/input/input3/event3").
// `kernel_env`: the kernel's KEY=VALUE strings (MAJOR, MINOR,
// DEVNAME=input/event3, ...) for group 1; `udev_env`: what udevd would
// broadcast after its rules (DEVNAME=/dev/input/event3, ID_INPUT=1, ...) for
// group 2, with `tags` ("seat", "uaccess"). Lists end with nullptr; any may
// be null. Process context only.
void uevent_broadcast(const char* action, const char* devpath, const char* subsystem, const char* const* kernel_env,
                      const char* const* udev_env, const char* const* tags);

} // namespace net
