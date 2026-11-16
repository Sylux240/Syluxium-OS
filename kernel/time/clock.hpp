#pragma once

#include <lib/types.hpp>

// Monotonic system clock (nanoseconds since clock::init()).
//
// Sources, best first: invariant TSC (calibrated), HPET. The PIT is only
// used to calibrate the TSC on machines without an HPET.
namespace clock {

void init();

u64 now_ns();
inline u64 now_ms() { return now_ns() / 1'000'000; }

void busy_wait_ns(u64 ns);

const char* source_name();
u64 tsc_frequency_hz();

} // namespace clock
