#pragma once

// Self-tests run at boot. Each one panics on failure, so reaching the
// "passed" line means the subsystem works on this machine.
namespace selftest {

void run_memory_tests();
void run_scheduler_tests();

} // namespace selftest
