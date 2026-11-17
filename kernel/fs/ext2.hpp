#pragma once

#include <block/block.hpp>
#include <fs/vfs.hpp>

// The ext2 / ext3 / ext4 family (Linux's native file systems).
//
// Reads all three, including ext4 extents and 64-bit layouts. Writes
// ext2-style volumes (block-mapped files, no metadata checksums); a volume
// using ext4 features the writer doesn't handle yet is mounted read-only.
namespace ext2 {

bool probe(block::Device* dev);
long mount(block::Device* dev, vfs::Inode** root, bool* read_only, const char** kind = nullptr);

} // namespace ext2
