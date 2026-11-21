#pragma once

#include <fs/vfs.hpp>

// cgroup2 file system (mount -t cgroup2): control files, nothing enforced yet.
namespace cgroupfs {
vfs::Inode* mount(); // a new hierarchy's root
}
