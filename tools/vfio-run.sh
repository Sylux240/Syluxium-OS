#!/bin/bash
# Boot Syluxium in QEMU with a real GPU passed through (VFIO), carefully:
#  - the card must look healthy before the run (vendor ID readable, bound to
#    vfio-pci), else nothing starts;
#  - QEMU's own display always stays (without it the VM firmware left the
#    card's register BAR unplaced, the guest kernel crashed, and the card hung
#    until the host rebooted);
#  - afterwards the card is checked again; if it no longer answers, the script
#    says so and refuses further runs until the host has rebooted.
# Usage: tools/vfio-run.sh [smoke-test.sh arguments...]   (env as smoke-test.sh:
#        DESKTOP, KCMD, TYPE, VM_RAM, VM_CPUS, QUIET, ...)
#        VFIO_ID=<vendor:device> picks the card (default 1002:73df, RX 6700 XT).
set -u
cd "$(dirname "$0")/.."
ID=${VFIO_ID:-1002:73df}
STUCK=build/vfio-card-stuck

GPU=$(lspci -D -d "$ID" | head -1 | cut -d' ' -f1)
[ -n "$GPU" ] || { echo "vfio-run: no $ID card on the PCI bus (it fell off: reboot the host)"; exit 1; }
AUDIO=${GPU%.0}.1

healthy() {
    local cfg=/sys/bus/pci/devices/$1/config
    [ -r "$cfg" ] || return 1
    local vend
    vend=$(od -An -tx2 -N2 "$cfg" | tr -d ' ')
    [ "$vend" = "${ID%%:*}" ] || return 1
    # Revision and subsystem too: after a VM the RX 6700 XT kept its vendor
    # ID but read revision 0xff and subsystem 0000:0000 (not reset; the
    # next VM's amdgpu crashed on it).
    local rev sub
    rev=$(od -An -tx1 -j8 -N1 "$cfg" | tr -d ' ')
    sub=$(od -An -tx4 -j44 -N4 "$cfg" | tr -d ' ')
    [ "$rev" != ff ] && [ "$sub" != 00000000 ] && [ "$sub" != ffffffff ] || return 1
    [ "$(basename "$(readlink "/sys/bus/pci/devices/$1/driver" 2>/dev/null)")" = vfio-pci ] || return 1
}

if [ -e "$STUCK" ]; then
    if [ "$(stat -c %Y "$STUCK")" -lt "$(date -d "$(uptime -s)" +%s)" ]; then
        rm -f "$STUCK" # the host rebooted since
    else
        echo "vfio-run: the card hung in an earlier run ($(cat "$STUCK")); reboot the host first"
        exit 1
    fi
fi
healthy "$GPU" || { echo "vfio-run: $GPU does not look healthy (not answering or not on vfio-pci): not starting"; exit 1; }
# Radeons survive only the first VM after a host boot unless reset in between
# (QEMU's reset at VM start fails on a card that was used): the host's
# amdgpu resets it properly (BACO). /usr/local/sbin/syluxium-gpu-reset, with
# a sudoers rule for just that script; without it, one VM run per boot.
if [ -x /usr/local/sbin/syluxium-gpu-reset ]; then
    sudo -n /usr/local/sbin/syluxium-gpu-reset || { echo "vfio-run: the host reset of $GPU failed: not starting"; exit 1; }
    healthy "$GPU" || { echo "vfio-run: $GPU not healthy after the host reset: not starting"; exit 1; }
elif [ -e build/vfio-card-used ] && [ "$(stat -c %Y build/vfio-card-used)" -ge "$(date -d "$(uptime -s)" +%s)" ]; then
    echo "vfio-run: the card was already used since the host booted and there's no reset helper: reboot first"
    exit 1
fi
touch build/vfio-card-used

case " ${QEMU_EXTRA:-} " in *" -vga none "*) echo "vfio-run: refusing -vga none (see the top of this script)"; exit 1 ;; esac
dev="-device vfio-pci,host=${GPU#0000:},multifunction=on"
[ -e "/sys/bus/pci/devices/$AUDIO" ] && dev="$dev -device vfio-pci,host=${AUDIO#0000:}"
export QEMU_EXTRA="${QEMU_EXTRA:-} $dev -global q35-pcihost.pci-hole64-size=64G -fw_cfg name=opt/ovmf/X-PciMmio64Mb,string=65536"

export POWERDOWN=1 # the guest powers off (drivers shut down) instead of QEMU being killed
tools/smoke-test.sh "${@:-uefi}"
rc=$?

sleep 2
if healthy "$GPU"; then
    echo "vfio-run: card $GPU still healthy after the run"
else
    echo "$(date '+%F %T') after: ${*:-uefi} ${KCMD:-}" > "$STUCK"
    echo "vfio-run: WARNING: card $GPU stopped answering after this run: reboot the host before the next one"
    rc=2
fi
exit $rc
