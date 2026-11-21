/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
struct dev_pagemap { int type; };
#define MEMORY_DEVICE_PRIVATE 1
#define MEMORY_DEVICE_COHERENT 2
