/* LinuxKPI: x86 memory barriers for virtualized devices (virtio). */
#pragma once
#include <linux/compiler.h>
#define virt_mb() mb()
#define virt_rmb() rmb()
#define virt_wmb() wmb()
#define virt_store_mb(var, value) do { WRITE_ONCE(var, value); mb(); } while (0)
#define virt_load_acquire(p) smp_load_acquire(p)
#define virt_store_release(p, v) smp_store_release(p, v)
#ifndef dma_mb
#define dma_mb() mb()
#endif
