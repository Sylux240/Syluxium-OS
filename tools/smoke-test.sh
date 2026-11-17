#!/usr/bin/env bash
# Boot the image headless, capture the serial log and a screenshot, then quit.
#
#   tools/smoke-test.sh [bios|uefi] [seconds] [label]
#
# Output: build/smoke/serial-<mode>.log and build/smoke/screen-<mode>.png
# With a label (for major tests), the screenshot and log are also archived to
#   smoke/<date>_<label>_<mode>.png / .log
# which lives outside build/ so `make clean` keeps it.
# Extra QEMU arguments can be passed in $QEMU_EXTRA (e.g. QEMU_EXTRA="-cpu max").
# USB=1 adds an xHCI controller with a USB keyboard, mouse and the FAT32
# stick from tools/make-test-disks.sh (as /dev/sdb).
# $TYPE: text typed on the (PS/2) keyboard after the boot wait, then a few
# more seconds pass before the screenshot. "\n" = Enter, "\c" = Ctrl+C, "\w" = wait 2 s, "\m" = mouse moves/clicks/scroll.
set -euo pipefail

MODE=${1:-bios}
SECONDS_TO_RUN=${2:-8}
LABEL=${3:-}
ARCHIVE=smoke
OUT=build/smoke
IMAGE=build/syluxium.img
OVMF=${OVMF:-/usr/share/edk2/x64/OVMF_CODE.4m.fd}

mkdir -p "$OUT"
SOCK="$OUT/monitor-$MODE.sock"
LOG="$OUT/serial-$MODE.log"
PPM="$OUT/screen-$MODE.ppm"
rm -f "$SOCK" "$LOG" "$PPM"

# Boot a private copy so this works even while `make run` has the image open.
# KEEP_DISK=1 reuses the copy from the previous run (persistence tests).
DISK="$OUT/disk-$MODE.img"
if [[ -z ${KEEP_DISK:-} || ! -f $DISK ]]; then
    cp "$IMAGE" "$DISK"
fi

args=(-M q35 -m 2G -smp 4 -cpu max
      -drive "file=$DISK,format=raw,if=ide"
      -display none -serial "file:$LOG"
      -monitor "unix:$SOCK,server,nowait"
      -no-reboot)
if [[ -n ${USB:-} ]]; then
    args+=(-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-mouse,bus=xhci.0
           -drive file=build/test-disks/usb-fat.img,if=none,id=stick,format=raw
           -device usb-storage,bus=xhci.0,drive=stick)
    [[ -f build/test-disks/usb-exfat.img ]] && args+=(
           -drive file=build/test-disks/usb-exfat.img,if=none,id=stick2,format=raw
           -device usb-storage,bus=xhci.0,drive=stick2)
fi
[[ -n ${QEMU_EXTRA:-} ]] && read -ra extra <<< "$QEMU_EXTRA" && args+=("${extra[@]}")
[[ $MODE == uefi ]] && args+=(-drive "if=pflash,format=raw,readonly=on,file=$OVMF")

qemu-system-x86_64 "${args[@]}" &
QEMU_PID=$!
trap 'kill $QEMU_PID 2>/dev/null || true' EXIT

sleep "$SECONDS_TO_RUN"

# Optionally type on the keyboard, then take a screenshot and quit.
python3 - "$SOCK" "$PPM" "${TYPE:-}" <<'EOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
names = {" ": "spc", "\n": "ret", "-": "minus", "=": "equal", ".": "dot", ",": "comma", "/": "slash",
         ";": "semicolon", "'": "apostrophe", "[": "bracket_left", "]": "bracket_right", "\\": "backslash",
         "`": "grave_accent"}
shifted = {"|": "backslash", "_": "minus", "+": "equal", ">": "dot", "<": "comma", "?": "slash", ":": "semicolon",
           '"': "apostrophe", "{": "bracket_left", "}": "bracket_right", "~": "grave_accent", "!": "1", "@": "2",
           "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0"}
text = sys.argv[3].replace("\\n", "\n").replace("\\c", "\x03").replace("\\w", "\x01").replace("\\m", "\x02")
for ch in text:
    if ch == "\x03":       # \c in $TYPE: Ctrl+C
        s.sendall(b"sendkey ctrl-c\n"); time.sleep(0.3); continue
    if ch == "\x01":       # \w in $TYPE: wait 2 seconds
        time.sleep(2); continue
    if ch == "\x02":       # \m in $TYPE: move the mouse, click, scroll
        for cmd in ["mouse_move 40 0", "mouse_move 0 25", "mouse_move -10 -5", "mouse_button 1", "mouse_button 0",
                    "mouse_button 2", "mouse_button 0", "mouse_move 0 0 -1", "mouse_move 0 0 -1"]:
            s.sendall((cmd + "\n").encode()); time.sleep(0.15)
        continue
    if ch in shifted:
        key = "shift-" + shifted[ch]
    elif ch.isupper():
        key = "shift-" + ch.lower()
    else:
        key = names.get(ch, ch)
    s.sendall(f"sendkey {key}\n".encode())
    time.sleep(0.06)
if text:
    time.sleep(6)
s.sendall(f"screendump {sys.argv[2]}\n".encode())
time.sleep(1)
s.sendall(b"quit\n")
time.sleep(0.5)
EOF
wait $QEMU_PID 2>/dev/null || true

python3 "$(dirname "$0")/ppm2png.py" "$PPM" "$OUT/screen-$MODE.png"
rm -f "$PPM"
echo "serial log: $LOG"
echo "screenshot: $OUT/screen-$MODE.png"

if [[ -n $LABEL ]]; then
    mkdir -p "$ARCHIVE"
    NAME="$ARCHIVE/$(date +%Y-%m-%d_%H%M)_${LABEL}_${MODE}"
    cp "$OUT/screen-$MODE.png" "$NAME.png"
    sed 's/\x1b\[[0-9;]*[A-Za-z]//g' "$LOG" > "$NAME.log"   # strip ANSI colors
    echo "archived:   $NAME.png"
fi
