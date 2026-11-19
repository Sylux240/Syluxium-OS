#pragma once

#include <fs/fat.hpp>
#include <block/block.hpp>
#include <fs/vfs.hpp>

// exFAT (read/write): large USB sticks, SD cards (SDXC), external drives
// shared with Windows and macOS. No 4 GiB file limit, unlike FAT32.
namespace exfat {

bool probe(block::Device* dev);
long mount(block::Device* dev, vfs::Inode** root, const fat::MountOptions& opts = {});

} // namespace exfat
