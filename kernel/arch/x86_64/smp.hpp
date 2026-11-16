#pragma once

#include <lib/types.hpp>

// Bringing up the other CPU cores ("application processors").
// Limine already did the INIT/SIPI dance and parked every AP; we hand each
// one a Cpu structure and an entry point, and it joins the scheduler.
namespace smp {

// Start all APs and wait (up to ~2 s) until they're online. Must run in a
// thread (it sleeps while waiting). Returns the number of CPUs online.
u32 start_aps();

} // namespace smp
