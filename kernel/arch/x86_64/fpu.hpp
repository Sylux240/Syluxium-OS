#pragma once

#include <lib/types.hpp>

// FPU / SSE / AVX register state.
//
// The kernel itself never touches these registers (it's compiled without
// SSE), so user state only needs saving when switching threads. We use
// XSAVE when available (covers AVX and AVX-512), FXSAVE otherwise.
namespace fpu {

void init_bsp();   // detect features, enable on the boot CPU, build the initial state
void init_ap();    // enable on another CPU (same feature set)

usize state_size();
void* alloc_state();          // fresh, default-initialized state (64-byte aligned)
void free_state(void* state);

void reset(void* state);      // back to the clean initial state (execve)
void sanitize(void* state);   // make user-supplied state safe to restore (sigreturn)

void save(void* state);
void restore(const void* state);

const char* mode_name();
u64 xcr0();

} // namespace fpu
