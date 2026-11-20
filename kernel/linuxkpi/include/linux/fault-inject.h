/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
struct fault_attr { int dummy; };
#define DECLARE_FAULT_ATTR(name) struct fault_attr name
#define should_fail(attr, size) false
#define fault_create_debugfs_attr(name, parent, attr) ((void *)0)
