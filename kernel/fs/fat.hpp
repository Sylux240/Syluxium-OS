#pragma once

#include <block/block.hpp>
#include <fs/vfs.hpp>

// FAT32 file system (read/write, long file names): USB sticks, SD cards,
// the EFI system partition, sharing files with Windows and macOS.
namespace fat {

// Mount the FAT file system on `dev`; returns its root directory inode.
// Linux vfat/exfat mount options: owner and permission masks for every
// file (FAT has no owners of its own).
struct MountOptions {
    u32 uid = 0, gid = 0;
    u32 fmask = 022, dmask = 022;
};
long mount(block::Device* dev, vfs::Inode** root, const MountOptions& opts = {});

// Quick check: does `dev` start with a FAT boot sector?
bool probe(block::Device* dev);

} // namespace fat
