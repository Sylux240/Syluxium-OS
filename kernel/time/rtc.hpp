#pragma once

#include <lib/types.hpp>

// Wall-clock time: read once from the CMOS real-time clock at boot, then
// advanced with the monotonic clock.
namespace rtc {

void init();
u64 boot_epoch_seconds();
u64 realtime_ns();   // nanoseconds since 1970-01-01 UTC

} // namespace rtc
