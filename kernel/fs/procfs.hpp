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

} // namespace sysfs
