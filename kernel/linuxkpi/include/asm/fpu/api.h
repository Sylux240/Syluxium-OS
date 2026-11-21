/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* Kernel FPU sections (arch fpu::kernel_begin/end, via core.cpp). */
void kernel_fpu_begin(void);
void kernel_fpu_end(void);
#define kernel_fpu_begin_mask(m) kernel_fpu_begin()
bool lkpi_irq_fpu_usable(void);
#define irq_fpu_usable() lkpi_irq_fpu_usable()
#define fpregs_lock() do { } while (0)
#define fpregs_unlock() do { } while (0)
