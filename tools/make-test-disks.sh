#!/usr/bin/env bash
# Build disk images for storage tests (no root needed: mkfs -d fills them).
#   build/test-disks/nvme-ext2.img  64 MiB ext2, read-write in Syluxium
#   build/test-disks/sata-ext4.img  64 MiB ext4 (extents, checksums), read-only in Syluxium
set -euo pipefail
OUT=build/test-disks
SRC=$OUT/content
rm -rf "$OUT"
mkdir -p "$SRC/docs/nested"
echo "Hello from the host, written by mkfs!" > "$SRC/hello.txt"
seq 1 60000 > "$SRC/docs/numbers.txt"            # ~350 KB: needs indirect blocks / extents
echo "deep file" > "$SRC/docs/nested/deep.txt"
ln -s docs/numbers.txt "$SRC/link-to-numbers"
ln -s "$(printf 'x%.0s' {1..80})" "$SRC/long-symlink"  # > 60 chars: stored in a data block
truncate -s 64M "$OUT/nvme-ext2.img" "$OUT/sata-ext4.img"
mkfs.ext2 -q -L syluxium-data -d "$SRC" "$OUT/nvme-ext2.img"
mkfs.ext4 -q -L host-ext4 -d "$SRC" "$OUT/sata-ext4.img"
echo "test disks ready in $OUT"
