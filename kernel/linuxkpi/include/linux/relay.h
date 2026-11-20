/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
struct rchan { size_t subbuf_size; size_t n_subbufs; struct rchan_buf **buf; };
struct rchan_buf { void *start; size_t subbufs_consumed; };
struct rchan_callbacks { int (*subbuf_start)(struct rchan_buf *, void *, void *, size_t); struct dentry *(*create_buf_file)(const char *, struct dentry *, umode_t, struct rchan_buf *, int *); int (*remove_buf_file)(struct dentry *); };
#define relay_open(base, parent, size, n, cb, priv) ((struct rchan *)NULL)
#define relay_close(c) do { } while (0)
#define relay_flush(c) do { } while (0)
#define relay_reserve(c, len) ((void *)NULL)
#define relay_late_setup_files(c, base, parent) (-ENODEV)
#define relay_file_operations (*(const struct file_operations *)NULL)
