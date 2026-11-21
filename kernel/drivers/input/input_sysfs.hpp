#pragma once

#include <drivers/input/input.hpp>

// Internal to the input layer: the /sys and udev-database view of the
// evdev devices (input_sysfs.cpp), fed by input.cpp.
namespace input::sysfs {

void init();                       // after /sys and /run exist
void added(u32 slot);              // /dev/input/event<slot> appeared
void removed(u32 slot);            // ... and went away

// input.cpp: a copy of the device in `slot` (false if none). `combined`:
// one of the kernel's combined devices (event0/event1, every keyboard /
// every mouse), which libinput must not use next to the real ones.
bool snapshot(u32 slot, Device* out, bool* combined);

} // namespace input::sysfs
