#pragma once

// Loop devices (/dev/loop-control, /dev/loopN): files used as disks.
namespace loop {
void create_nodes(); // /dev/loop-control and /dev/loop0..7
// A file system mounted from `source` went away: release it if it is a loop
// device set up with LO_FLAGS_AUTOCLEAR (mount -o loop does that).
void unmounted(const char* source);
}
