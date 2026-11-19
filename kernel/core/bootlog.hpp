#pragma once

// Saves the kernel log onto the boot medium (syluxium-log.txt next to the
// boot/ folder of the Syluxium boot partition), so a machine where the
// keyboard or display doesn't work still leaves a log on the USB stick.
// Only written to a partition that holds boot/syluxium.elf - never to
// another system's EFI partition.
namespace bootlog {

void start(); // saves at intervals during the first 10 minutes
void save();  // now (also at shutdown/restart)

} // namespace bootlog
