/* LinuxKPI: pseudo file system contexts. */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/fs.h>
struct pseudo_fs_context { const struct super_operations *ops; const struct xattr_handler **xattr; const struct dentry_operations *dops; unsigned long magic; };
struct pseudo_fs_context *init_pseudo(struct fs_context *fc, unsigned long magic);
