#!/bin/sh
# Builds NVIDIA's kernel modules (Debian trixie's nvidia-kernel-dkms, the
# proprietary driver: GeForce 700 Maxwell/900 series and newer up to RTX 40)
# for Syluxium's kernel, in a Debian trixie container (Docker), the way DKMS
# builds them on a Debian system. The image then needs no compiler or headers.
#   tools/nvidia-modules.sh <kernel version> <output dir>
# Output: <output dir>/nvidia*.ko.xz and VERSION (the driver's version).
set -e
kver=$1
out=$2
[ -n "$kver" ] && [ -n "$out" ] || { echo "usage: $0 <kernel version> <output dir>" >&2; exit 1; }
mkdir -p "$out"
out=$(cd "$out" && pwd)
docker run --rm -v "$out:/out" debian:trixie sh -ec "
    sed -i 's/^Components: main.*/Components: main contrib non-free non-free-firmware/' /etc/apt/sources.list.d/debian.sources
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        linux-headers-$kver nvidia-kernel-dkms > /dev/null
    ver=\$(dpkg-query -W -f='\${Version}' nvidia-kernel-dkms | cut -d- -f1)
    dkms build nvidia-current/\$ver -k $kver > /dev/null || { cat /var/lib/dkms/nvidia-current/\$ver/build/make.log | tail -40; exit 1; }
    dkms install nvidia-current/\$ver -k $kver --force > /dev/null
    find /lib/modules/$kver/updates -name 'nvidia*.ko*' -exec cp {} /out/ \;
    echo \$ver > /out/VERSION
    chown -R $(id -u):$(id -g) /out
"
ls "$out"
