#!/usr/bin/env bash
# USB hot-plug test: plugs and unplugs devices through the QEMU monitor while
# the system runs.  tools/usb-hotplug-test.sh [bios|uefi] [label]  (needs tools/make-test-disks.sh)
# Controller 1 (xhci, all root ports busy): new devices land behind QEMU's hub (hub status-change path).
# Controller 2 (x2, empty): root-port Port Status Change events, USB 2 and SuperSpeed ports.
set -euo pipefail
cp build/test-disks/usb-fat.img build/test-disks/usb-hot.img
cp build/test-disks/usb-fat.img build/test-disks/usb-hot2.img
T='ls /dev/input /dev/sd*\n'
T+='\{device_add usb-kbd,bus=xhci.0,id=k2}\w'
T+='\{device_add usb-tablet,bus=xhci.0,id=tab}\w'
T+='\{drive_add 0 if=none,id=hot,file=build/test-disks/usb-hot.img,format=raw}'
T+='\{device_add usb-storage,bus=xhci.0,drive=hot,id=s3}\w\w'
T+='\{device_add usb-kbd,bus=x2.0,id=rk}\w'
T+='\{drive_add 0 if=none,id=hot2,file=build/test-disks/usb-hot2.img,format=raw}'
T+='\{device_add usb-storage,bus=x2.0,drive=hot2,id=s4}\w\w'
T+='ls /dev/input /dev/sd*\n\w'
T+='mount /dev/sdd /mnt && cat /mnt/README.TXT && umount /mnt\n\w'
T+='\{device_del k2}\w\{device_del s3}\w\{device_del rk}\w\{device_del s4}\w\w'
T+='ls /dev/input /dev/sd*\n\w'
T+='\{device_add usb-kbd,bus=x2.0,id=rk2}\w\w'
T+='\{device_del tab}\w\w'
T+='ls /dev/input /dev/sd*\n'
TYPE="$T" USB=1 QEMU_EXTRA="-device qemu-xhci,id=x2" tools/smoke-test.sh "${1:-bios}" 90 ${2:-}
