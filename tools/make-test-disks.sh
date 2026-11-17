#!/usr/bin/env bash
# Build disk images for storage tests (no root needed: mkfs -d fills them).
#   build/test-disks/nvme-ext2.img  64 MiB ext2, read-write in Syluxium
#   build/test-disks/sata-ext4.img  64 MiB ext4 (extents, checksums), read-only in Syluxium
#   build/test-disks/usb-fat.img   48 MiB FAT32 USB stick
#   build/test-disks/usb-exfat.img 64 MiB exFAT USB stick (second stick: /dev/sdc)
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

# USB stick: FAT32 (MBR-less "superfloppy", like many sticks) with a file.
rm -f "$OUT/usb-fat.img"
truncate -s 48M "$OUT/usb-fat.img"
mkfs.fat -F 32 -n USBSTICK "$OUT/usb-fat.img" >/dev/null
echo "hello from a USB stick" > "$OUT/usb-readme.txt"
mcopy -i "$OUT/usb-fat.img" "$OUT/usb-readme.txt" ::/README.TXT
echo "built $OUT/usb-fat.img"

# exFAT stick (mkfs.exfat from exfatprogs). exfatprogs can't pre-fill a
# volume without mounting it, so it starts empty: Syluxium writes to it and
# the host's fsck.exfat checks the result.
rm -f "$OUT/usb-exfat.img"
truncate -s 64M "$OUT/usb-exfat.img"
mkfs.exfat -q -L EXSTICK "$OUT/usb-exfat.img" >/dev/null 2>&1 || mkfs.exfat -n EXSTICK "$OUT/usb-exfat.img" >/dev/null
echo "built $OUT/usb-exfat.img"
