#pragma once

#include <lib/types.hpp>

// Pieces of <new> that the compiler expects to exist.
namespace std {
enum class align_val_t : usize {};
}

// Placement new.
inline void* operator new(usize, void* where) noexcept { return where; }
inline void* operator new[](usize, void* where) noexcept { return where; }

namespace cxx {

// Run constructors of global/static objects (.init_array).
void run_global_constructors();

} // namespace cxx
