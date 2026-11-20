/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/random.h>
struct rnd_state { u32 s1, s2, s3, s4; };
static inline void prandom_seed_state(struct rnd_state *state, u64 seed)
{
	u32 i = ((seed >> 32) ^ (seed << 10) ^ seed) & 0xffffffffUL;
	state->s1 = i + 2; state->s2 = i + 8; state->s3 = i + 16; state->s4 = i + 128;
}
static inline u32 prandom_u32_state(struct rnd_state *state)
{
#define TAUSWORTHE(s, a, b, c, d) ((s & c) << d) ^ (((s << a) ^ s) >> b)
	state->s1 = TAUSWORTHE(state->s1, 6U, 13U, 4294967294U, 18U);
	state->s2 = TAUSWORTHE(state->s2, 2U, 27U, 4294967288U, 2U);
	state->s3 = TAUSWORTHE(state->s3, 13U, 21U, 4294967280U, 7U);
	state->s4 = TAUSWORTHE(state->s4, 3U, 12U, 4294967168U, 13U);
#undef TAUSWORTHE
	return state->s1 ^ state->s2 ^ state->s3 ^ state->s4;
}
