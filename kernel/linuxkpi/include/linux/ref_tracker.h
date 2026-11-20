/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: reference tracking (debug only): off. */
struct ref_tracker;
struct ref_tracker_dir { int dummy; };
#define ref_tracker_dir_init(dir, quarantine, name) do { } while (0)
#define ref_tracker_dir_exit(dir) do { } while (0)
#define ref_tracker_dir_print(dir, n) do { } while (0)
#define ref_tracker_dir_print_locked(dir, n) do { } while (0)
#define ref_tracker_dir_snprint(dir, buf, size) 0
#define ref_tracker_alloc(dir, trackerp, gfp) 0
#define ref_tracker_free(dir, trackerp) 0
