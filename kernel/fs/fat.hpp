#pragma once

#include <block/block.hpp>
#include <fs/vfs.hpp>

// FAT32 file system (read/write, long file names): USB sticks, SD cards,
// the EFI system partition, sharing files with Windows and macOS.
namespace fat {

// Mount the FAT file system on `dev`; returns its root directory inode.
long mount(block::Device* dev, vfs::Inode** root);

// Quick check: does `dev` start with a FAT boot sector?
bool probe(block::Device* dev);

} // namespace fat
