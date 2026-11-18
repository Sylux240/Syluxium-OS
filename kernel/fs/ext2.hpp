#pragma once

#include <block/block.hpp>
#include <fs/vfs.hpp>

// The ext2 / ext3 / ext4 family (Linux's native file systems).
//
// Reads and writes all three, including ext4 as Debian's mkfs.ext4 makes it:
// extent trees, 64-bit block groups, flex_bg, uninitialized groups, metadata
// checksums (crc32c). Directory htree indexes are read, and an indexed
// directory turns linear when changed. Not yet: journaling (writes go
// straight to their place; a journal that needs replaying -> read-only),
// meta_bg, inline data, encryption.
namespace ext2 {

bool probe(block::Device* dev);
long mount(block::Device* dev, vfs::Inode** root, bool* read_only, const char** kind = nullptr);

} // namespace ext2
