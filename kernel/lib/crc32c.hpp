#pragma once

#include <lib/types.hpp>

// CRC-32C (Castagnoli), as used by ext4 metadata checksums, jbd2, iSCSI,
// btrfs. "Raw" form like Linux's crc32c(): no initial or final inversion
// is applied here; callers pass ~0 as the seed where the format asks for it.
u32 crc32c(u32 crc, const void* data, usize len);
