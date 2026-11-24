#!/usr/bin/env python3
"""Build Syluxium's small initrd for the Linux kernel.

It only has to find the system partition (root=LABEL=SYLUXLIVE on a live
stick, root=UUID=... on an installed disk), mount it and switch to it: busybox
and kmod with their libraries, the storage and file system drivers (USB,
SATA/AHCI, NVMe, SD cards, virtio for VMs, ext4), and a small /init.

    tools/mkinitrd.py <rootfs> <kernel version> <output initrd.img>
"""
import os
import shutil
import subprocess
import sys

LIB_DIRS = ["usr/lib/x86_64-linux-gnu", "usr/lib64", "lib64", "usr/lib", "lib/x86_64-linux-gnu"]

# Module directories (under lib/modules/<ver>/kernel) whose drivers can sit
# between the firmware and the system partition.
MODULE_DIRS = [
    "drivers/ata", "drivers/nvme", "drivers/scsi", "drivers/block", "drivers/virtio",
    "drivers/usb/host", "drivers/usb/storage", "drivers/usb/core", "drivers/usb/common",
    "drivers/mmc", "drivers/hid", "drivers/input/keyboard", "drivers/md",
    "fs/ext4", "fs/jbd2", "fs/mbcache.ko.xz", "fs/fat", "fs/nls",
    "lib", "crypto", "arch/x86/crypto",
]

# Loaded in this order before looking for the system partition (the rest
# comes from the devices' own modaliases).
EARLY_MODULES = [
    "crc32c_generic", "crc32c_intel", "ext4", "vfat", "nls_cp437", "nls_iso8859_1",
    "virtio_pci", "virtio_blk", "virtio_scsi", "ahci", "ata_piix", "nvme",
    "xhci_pci", "ehci_pci", "ohci_pci", "uhci_hcd", "usb_storage", "uas", "sd_mod",
    "sdhci_pci", "mmc_block", "usbhid", "hid_generic",
]

INIT = r"""#!/bin/sh
# Syluxium initrd: find the system partition, mount it, switch to it.
export PATH=/bin
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev
root=
for w in $(cat /proc/cmdline); do
    case "$w" in root=*) root=${w#root=} ;; esac
done
[ -n "$root" ] || root=LABEL=SYLUXLIVE
for m in $(cat /etc/early-modules); do modprobe -q "$m" 2>/dev/null; done
coldplug() { # drivers for whatever has appeared (USB sticks show up late)
    cat $(find /sys/devices -name modalias 2>/dev/null) 2>/dev/null | sort -u | xargs modprobe -q -a 2>/dev/null
}
dev=
i=0
while [ $i -lt 120 ]; do
    coldplug
    dev=$(findfs "$root" 2>/dev/null) && [ -n "$dev" ] && break
    [ $i = 10 ] && echo "Syluxium: waiting for the system partition ($root)..."
    sleep 0.25
    i=$((i + 1))
done
if [ -z "$dev" ]; then
    echo "Syluxium: the system partition ($root) was not found."
    echo "Disks seen:"; cat /proc/partitions
    echo "A shell follows (type 'exit' to try again)."
    sh
    exec /init
fi
mkdir -p /newroot
if ! mount -o rw "$dev" /newroot; then
    echo "Syluxium: $dev ($root) could not be mounted. A shell follows."
    sh
    exec /init
fi
mount --move /dev /newroot/dev
umount /sys /proc
exec switch_root /newroot /sbin/init
"""


def needed(path):
    out = subprocess.run(["readelf", "-d", path], capture_output=True, text=True).stdout
    libs = []
    for line in out.splitlines():
        if "(NEEDED)" in line and "[" in line:
            libs.append(line.split("[", 1)[1].split("]", 1)[0])
    return libs


def find_lib(rootfs, name):
    for d in LIB_DIRS:
        p = os.path.join(rootfs, d, name)
        if os.path.exists(p):
            return p
    return None


def copy_with_libs(rootfs, stage, rel, seen):
    real = os.path.realpath(os.path.join(rootfs, rel))
    dst = os.path.join(stage, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(real, dst)
    for lib in needed(real):
        if lib in seen:
            continue
        seen.add(lib)
        p = find_lib(rootfs, lib)
        if not p:
            sys.exit(f"mkinitrd: library {lib} (for {rel}) not found in {rootfs}")
        shutil.copy2(os.path.realpath(p), os.path.join(stage, "lib", lib))
        copy_with_libs_lib(rootfs, stage, os.path.realpath(p), seen)


def copy_with_libs_lib(rootfs, stage, real, seen):
    for lib in needed(real):
        if lib in seen:
            continue
        seen.add(lib)
        p = find_lib(rootfs, lib)
        if not p:
            sys.exit(f"mkinitrd: library {lib} not found in {rootfs}")
        out = os.path.join(stage, "lib", lib)
        shutil.copy2(os.path.realpath(p), out)
        copy_with_libs_lib(rootfs, stage, os.path.realpath(p), seen)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    rootfs, kver, output = sys.argv[1:]
    stage = output + ".stage"
    shutil.rmtree(stage, ignore_errors=True)
    for d in ("bin", "lib", "etc", "proc", "sys", "dev", "newroot", "tmp"):
        os.makedirs(os.path.join(stage, d))
    # The dynamic loader where binaries look for it.
    os.makedirs(os.path.join(stage, "lib64"))
    ld = find_lib(rootfs, "ld-linux-x86-64.so.2")
    shutil.copy2(os.path.realpath(ld), os.path.join(stage, "lib64", "ld-linux-x86-64.so.2"))
    seen = {"ld-linux-x86-64.so.2"}
    for rel in ("usr/bin/busybox", "usr/bin/kmod"):
        copy_with_libs(rootfs, stage, rel, seen)
        shutil.move(os.path.join(stage, rel), os.path.join(stage, "bin", os.path.basename(rel)))
    shutil.rmtree(os.path.join(stage, "usr"), ignore_errors=True)
    applets = subprocess.run([os.path.join(rootfs, "usr/bin/busybox"), "--list"], capture_output=True, text=True).stdout.split()
    for a in applets:
        if a not in ("busybox", "modprobe", "insmod", "rmmod", "lsmod", "depmod", "modinfo"):
            os.symlink("busybox", os.path.join(stage, "bin", a))
    for a in ("modprobe", "insmod", "rmmod", "lsmod", "depmod", "modinfo"):
        os.symlink("kmod", os.path.join(stage, "bin", a))
    with open(os.path.join(stage, "etc", "ld.so.conf"), "w") as f:
        f.write("/lib\n")
    # glibc finds the libraries through the loader's default path or LD_LIBRARY_PATH:
    # the loader's defaults include /lib, which is where they are.
    moddir = os.path.join("usr/lib/modules", kver)
    src_mod = os.path.join(rootfs, moddir)
    dst_mod = os.path.join(stage, "lib/modules", kver)
    os.makedirs(dst_mod)
    for f in ("modules.order", "modules.builtin", "modules.builtin.modinfo"):
        if os.path.exists(os.path.join(src_mod, f)):
            shutil.copy2(os.path.join(src_mod, f), dst_mod)
    count = 0
    for d in MODULE_DIRS:
        s = os.path.join(src_mod, "kernel", d)
        t = os.path.join(dst_mod, "kernel", d)
        if os.path.isdir(s):
            shutil.copytree(s, t, dirs_exist_ok=True)
            count += sum(len(files) for _, _, files in os.walk(t))
        elif os.path.isfile(s):
            os.makedirs(os.path.dirname(t), exist_ok=True)
            shutil.copy2(s, t)
            count += 1
    subprocess.run(["depmod", "-b", stage, kver], check=True)
    with open(os.path.join(stage, "etc", "early-modules"), "w") as f:
        f.write("\n".join(EARLY_MODULES) + "\n")
    with open(os.path.join(stage, "init"), "w") as f:
        f.write(INIT)
    os.chmod(os.path.join(stage, "init"), 0o755)
    cpio = subprocess.Popen(f"cd '{stage}' && find . | LC_ALL=C sort | cpio -o -H newc --quiet --owner=0:0 | gzip -n -9",
                            shell=True, stdout=open(output + ".tmp", "wb"))
    if cpio.wait() != 0:
        sys.exit("mkinitrd: cpio failed")
    os.replace(output + ".tmp", output)
    shutil.rmtree(stage)
    print(f"  INITRD {output}: {count} modules, {os.path.getsize(output) // 1024} KiB")


if __name__ == "__main__":
    main()
