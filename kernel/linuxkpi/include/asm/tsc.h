/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
unsigned long lkpi_tsc_khz(void);
#define tsc_khz lkpi_tsc_khz()
static inline u64 rdtsc(void) { u32 lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((u64)hi << 32) | lo; }
#define rdtsc_ordered() rdtsc()
#define get_cycles() rdtsc()
