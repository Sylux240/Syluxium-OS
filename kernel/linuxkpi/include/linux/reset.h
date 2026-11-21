/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
struct reset_control;
#define devm_reset_control_get(dev, id) ((struct reset_control *)ERR_PTR(-ENODEV))
#define reset_control_assert(r) 0
#define reset_control_deassert(r) 0
