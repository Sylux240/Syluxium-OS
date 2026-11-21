/* LinuxKPI: sort() (lib/sort.c) */
#pragma once
#include <linux/lkpi_all.h>
void sort(void *base, size_t num, size_t size, int (*cmp)(const void *, const void *), void (*swap)(void *, void *, int));
