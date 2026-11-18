#pragma once

#include <block/block.hpp>
#include <fs/vfs.hpp>

// The ext2 / ext3 / ext4 family (Linux's native file systems).
//
// Reads and writes all three, including ext4 as Debian's mkfs.ext4 makes it:
// extent trees, 64-bit block groups, flex_bg, uninitialized groups, metadata
// checksums (crc32c), and the jbd2 journal: metadata changes are committed
// to the journal (ordered mode: file data first) every few seconds and on
// sync, a journal left by a crash is replayed at mount. Directory htree
// indexes are read, and an indexed directory turns linear when changed.
// Not yet: meta_bg, inline data, encryption, external journals.
namespace ext2 {

bool probe(block::Device* dev);
long mount(block::Device* dev, vfs::Inode** root, bool* read_only, const char** kind = nullptr);

// Shutting down: commit journals, mark every writable volume clean (no
// replay needed at the next mount) and flush it to the disk.
void shutdown_all();

} // namespace ext2
