#pragma once

#include <lib/types.hpp>

// Kernel random numbers: RDRAND when the CPU has it, otherwise a xorshift
// generator seeded from the TSC. NOT yet a proper CSPRNG (no entropy pool);
// fine for AT_RANDOM / ASLR seeds / tests, to be replaced by a ChaCha20 RNG.
namespace random {

void init();
u64 next_u64();
void fill(void* buf, usize len);
bool hardware();

} // namespace random
