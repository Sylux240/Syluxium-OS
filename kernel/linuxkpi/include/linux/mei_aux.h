/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/auxiliary_bus.h>
struct mei_aux_device { struct auxiliary_device aux_dev; int irq; struct resource bar; struct resource ext_op_mem; bool slow_firmware; };
