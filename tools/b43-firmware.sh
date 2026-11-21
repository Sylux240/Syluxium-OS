#!/bin/bash
# Firmware for Broadcom b43 Wi-Fi (BCM4331 and friends in older MacBooks and
# iMacs), put into the image at build time: Broadcom's driver archive (the
# one Debian's firmware-b43-installer uses, checked against its SHA-512) is
# downloaded once into deps/b43, and b43-fwcutter cuts the firmware out of it.
# Without it the card can't come up, and without a connection the system
# can't fetch it later. If the download fails the build carries on, and
# syluxium-b43-firmware can still fetch it on a machine that has a cable.
#   tools/b43-firmware.sh <rootfs>
set -u
ROOTFS=$1
VERSION=6.30.163.46
TARBALL=broadcom-wl-$VERSION.tar.bz2
WL_APSTA=broadcom-wl-$VERSION.wl_apsta.o
URL=https://github.com/minios-linux/b43-firmware/releases/download/b43-firmware/$TARBALL
SHA512=0144894fbbb5e8ebab6c423d9bd0f3249be94f2f468a50b8bf721a3b17f1f6e57467c79e87abc8d136bfc92e701ed046885fead892e9a73efa5217d710311ae9
CACHE=deps/b43
FW=$ROOTFS/usr/lib/firmware

mkdir -p "$CACHE"
if ! echo "$SHA512  $CACHE/$TARBALL" | sha512sum -c --status 2>/dev/null; then
    curl -fsSL -o "$CACHE/$TARBALL.part" "$URL" && mv "$CACHE/$TARBALL.part" "$CACHE/$TARBALL"
    if ! echo "$SHA512  $CACHE/$TARBALL" | sha512sum -c --status 2>/dev/null; then
        echo "  warning: b43 firmware download failed or checksum mismatch; not in the image"
        rm -f "$CACHE/$TARBALL"
        exit 0
    fi
fi
[ -f "$CACHE/$WL_APSTA" ] || tar xjf "$CACHE/$TARBALL" -C "$CACHE" "$WL_APSTA" || exit 0
"$ROOTFS/usr/bin/b43-fwcutter" -w "$FW" "$CACHE/$WL_APSTA" >/dev/null || { echo "  warning: b43-fwcutter failed"; exit 0; }
echo "  b43 firmware: $(ls "$FW/b43" | wc -l) files"
