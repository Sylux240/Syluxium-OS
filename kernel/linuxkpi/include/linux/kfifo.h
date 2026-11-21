/* LinuxKPI: kfifo - the fixed-size, typed FIFOs (DECLARE_KFIFO/INIT_KFIFO,
 * kfifo_put/get/len/is_empty/reset). Size must be a power of two. Not
 * lockless-safe beyond one producer and one consumer, as in Linux. */
#pragma once
#include <linux/lkpi_all.h>

#define DECLARE_KFIFO(fifo, type, size) \
	struct { unsigned int in, out, mask; type buf[size]; } fifo
#define INIT_KFIFO(fifo) do { (fifo).in = (fifo).out = 0; (fifo).mask = ARRAY_SIZE((fifo).buf) - 1; } while (0)
#define kfifo_len(f) ((f)->in - (f)->out)
#define kfifo_is_empty(f) ((f)->in == (f)->out)
#define kfifo_is_full(f) (kfifo_len(f) > (f)->mask)
#define kfifo_reset(f) do { (f)->in = (f)->out = 0; } while (0)
#define kfifo_size(f) ((f)->mask + 1)
#define kfifo_put(f, val) ({ int __ok = !kfifo_is_full(f); \
	if (__ok) { (f)->buf[(f)->in & (f)->mask] = (val); smp_wmb(); (f)->in++; } __ok; })
#define kfifo_get(f, ptr) ({ int __ok = !kfifo_is_empty(f); \
	if (__ok) { *(ptr) = (f)->buf[(f)->out & (f)->mask]; smp_wmb(); (f)->out++; } __ok; })
#define kfifo_peek(f, ptr) ({ int __ok = !kfifo_is_empty(f); if (__ok) *(ptr) = (f)->buf[(f)->out & (f)->mask]; __ok; })

/* FIFOs whose buffer is allocated at run time (DECLARE_KFIFO_PTR + kfifo_alloc). */
#define DECLARE_KFIFO_PTR(fifo, type) \
	struct { unsigned int in, out, mask; type *buf; } fifo
#define kfifo_alloc(f, size, gfp) ({ unsigned int __n = roundup_pow_of_two(size); \
	(f)->in = (f)->out = 0; (f)->mask = __n - 1; \
	(f)->buf = kcalloc(__n, sizeof(*(f)->buf), gfp); (f)->buf ? 0 : -ENOMEM; })
#define kfifo_free(f) do { kfree((f)->buf); (f)->buf = NULL; (f)->mask = 0; } while (0)
