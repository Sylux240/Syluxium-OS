/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
struct debug_obj_descr { const char *name; };
#define debug_object_init(addr, descr) do { } while (0)
#define debug_object_activate(addr, descr) 0
#define debug_object_deactivate(addr, descr) do { } while (0)
#define debug_object_destroy(addr, descr) do { } while (0)
#define debug_object_free(addr, descr) do { } while (0)
#define debug_object_assert_init(addr, descr) do { } while (0)
#define debug_object_init_on_stack(addr, descr) do { } while (0)
#define debug_object_active_state(addr, descr, e, n) do { } while (0)
