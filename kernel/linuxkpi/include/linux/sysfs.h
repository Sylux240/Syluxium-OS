/* LinuxKPI (GPU drivers) */
#pragma once
#include <linux/lkpi_all.h>
/* LinuxKPI: sysfs - attributes are accepted and not exposed. */
struct kobject;
struct kobj_attribute { struct attribute attr; ssize_t (*show)(struct kobject *, struct kobj_attribute *, char *); ssize_t (*store)(struct kobject *, struct kobj_attribute *, const char *, size_t); };
#define sysfs_create_group(k, g) 0
#define sysfs_remove_group(k, g) do { } while (0)
#define sysfs_create_groups(k, g) 0
#define sysfs_remove_groups(k, g) do { } while (0)
#define sysfs_create_file(k, a) 0
#define sysfs_remove_file(k, a) do { } while (0)
#define sysfs_create_files(k, a) 0
#define sysfs_remove_files(k, a) do { } while (0)
#define sysfs_create_bin_file(k, a) 0
#define sysfs_remove_bin_file(k, a) do { } while (0)
#define sysfs_merge_group(k, g) 0
#define sysfs_unmerge_group(k, g) do { } while (0)
#define sysfs_notify(k, d, a) do { } while (0)
#define sysfs_emit_at(buf, at, fmt, ...) snprintf((buf) + (at), PAGE_SIZE - (at), fmt, ##__VA_ARGS__)
#define sysfs_streq(a, b) (strcmp(a, b) == 0)
#define sysfs_attr_init(a) do { } while (0)
