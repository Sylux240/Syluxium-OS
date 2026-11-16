#pragma once

// Device nodes in /dev: null, zero, full, random, urandom, kmsg, console/tty.
namespace devices {

void init(); // needs vfs::init() and tty::init() first

} // namespace devices
