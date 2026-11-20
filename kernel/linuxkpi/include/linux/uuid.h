/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
typedef struct { u8 b[16]; } guid_t;
typedef struct { u8 b[16]; } uuid_t;
#define UUID_SIZE 16
#define GUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) ((guid_t){{ (a) & 0xff, ((a) >> 8) & 0xff, ((a) >> 16) & 0xff, ((a) >> 24) & 0xff, (b) & 0xff, ((b) >> 8) & 0xff, (c) & 0xff, ((c) >> 8) & 0xff, (d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) }})
#define UUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) ((uuid_t){{ ((a) >> 24) & 0xff, ((a) >> 16) & 0xff, ((a) >> 8) & 0xff, (a) & 0xff, ((b) >> 8) & 0xff, (b) & 0xff, ((c) >> 8) & 0xff, (c) & 0xff, (d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) }})
static inline bool guid_equal(const guid_t *u1, const guid_t *u2) { return __builtin_memcmp(u1, u2, sizeof(guid_t)) == 0; }
static inline bool uuid_equal(const uuid_t *u1, const uuid_t *u2) { return __builtin_memcmp(u1, u2, sizeof(uuid_t)) == 0; }
static inline void guid_copy(guid_t *dst, const guid_t *src) { __builtin_memcpy(dst, src, sizeof(guid_t)); }
static inline void uuid_copy(uuid_t *dst, const uuid_t *src) { __builtin_memcpy(dst, src, sizeof(uuid_t)); }
#define guid_is_null(g) false
#define uuid_is_null(u) false
#define guid_gen(g) get_random_bytes(g, sizeof(guid_t))
#define uuid_gen(u) get_random_bytes(u, sizeof(uuid_t))
