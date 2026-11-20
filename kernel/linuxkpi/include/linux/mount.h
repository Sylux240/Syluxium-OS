/* LinuxKPI: internal mounts of pseudo file systems (dma-buf's "dmabuf"). */
#pragma once
#include <linux/lkpi_all.h>
#include <linux/fs.h>
struct vfsmount { struct super_block *mnt_sb; };
struct vfsmount *kern_mount(struct file_system_type *type);
static inline void kern_unmount(struct vfsmount *mnt) { (void)mnt; }
