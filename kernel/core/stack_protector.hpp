#pragma once

#include <lib/types.hpp>

namespace stack_protector {

// Pick a random canary. Must run before any function with a canary is
// entered, apart from the never-returning entry point that calls it.
void init();

} // namespace stack_protector
