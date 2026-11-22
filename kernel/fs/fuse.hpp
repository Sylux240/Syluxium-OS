#pragma once

#include <fs/vfs.hpp>

// FUSE: file systems served by a user-space program over /dev/fuse (Linux's
// protocol, 7.31), so libfuse 2/3, fusermount3 and the programs built on
// them run unmodified: AppImages (squashfuse inside their runtime), sshfs,
// gvfs-fuse, ...
namespace fuse {

void create_device(); // /dev/fuse (misc 10:229)

// mount(2) with type "fuse", "fuse.<subtype>" or "fuseblk". `data` is the
// option string fusermount passes (fd=N,rootmode=O,user_id=U,group_id=G,
// allow_other, default_permissions, max_read=N, ...). On success *root is
// the file system's root and `shown` the options /proc/mounts lists.
long mount(const char* data, vfs::Inode** root, char* shown, usize shown_size);

// After umount: the connection ends (the server's read() returns ENODEV and
// libfuse's loop exits). `root` is what the mount point resolved to.
void unmounted(vfs::Inode* root);

} // namespace fuse
