#pragma once

#include <lib/types.hpp>

// Unpacks a "newc" cpio archive (the Linux initramfs format) into the root
// filesystem.
namespace initramfs {

struct Stats {
    u32 files, dirs, symlinks;
    u64 bytes;
};

// Returns a negative errno on a malformed archive.
long unpack(const void* archive, usize size, Stats* stats);

} // namespace initramfs
