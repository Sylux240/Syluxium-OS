#pragma once

#include <lib/types.hpp>

// DEFLATE (RFC 1951) inside a gzip member (RFC 1952): decompresses the
// initramfs. Output is pushed to `sink` in pieces of at most 32 KiB as it
// is produced, so nothing the size of the whole output is ever allocated.
namespace inflate {

using Sink = bool (*)(void* ctx, const u8* data, usize len); // false: stop

// Returns 0, or a negative errno on corrupt data / when the sink stops.
long gunzip(const void* data, usize len, Sink sink, void* ctx);

bool is_gzip(const void* data, usize len);

} // namespace inflate
