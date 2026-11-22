#!/usr/bin/env bash
# Boot the image headless, capture the serial log and a screenshot, then quit.
#
#   tools/smoke-test.sh [bios|uefi] [seconds] [label]
#
# Output: build/smoke/serial-<mode>.log and build/smoke/screen-<mode>.png
# With a label (for major tests), the screenshot and log are also archived to
#   smoke/<date>_<label>_<mode>.png / .log
# which lives outside build/ so `make clean` keeps it.
# <seconds> is the maximum wait for the shell prompt; typing starts as soon as it
# appears, and the run ends once the output has been quiet for 3 s (QUIET=<secs>
# to wait longer, for commands that are silent for a while).
# Extra QEMU arguments can be passed in $QEMU_EXTRA (e.g. QEMU_EXTRA="-cpu max").
# USB=1 adds an xHCI controller with a USB keyboard, mouse and the FAT32
# stick from tools/make-test-disks.sh (as /dev/sdb).
# $TYPE: text typed on the (PS/2) keyboard after the boot wait, then a few
# more seconds pass before the screenshot. "\n" = Enter, "\c" = Ctrl+C, "\w" = wait 2 s, "\m" = mouse moves/clicks/scroll, "\s" = Alt+SysRq+W (thread dump), "\o" = Alt+F1 (launcher menu), "\e" = Ctrl+Esc (start menu), "\t" = Ctrl+Alt+Del (Task Manager),
# "\{cmd}" = run a QEMU monitor command (e.g. "\{device_add usb-kbd,bus=xhci.0,id=k2}" for hot-plug tests),
# "\[text]" = wait until the serial log shows <text> (at most MARK_WAIT seconds, default 180): a test
# inside the system says when it's ready for keys or a screenshot ("\{screendump build/smoke/x.ppm}").
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
# BOOTDISK=<img> boots that disk image as it is (an installed system).
if [[ -n ${BOOTDISK:-} ]]; then
    DISK=$BOOTDISK
elif [[ -z ${KEEP_DISK:-} || ! -f $DISK ]]; then
    cp "$IMAGE" "$DISK"
    # Tests type at the text console, so boot the "text console" entry
    # unless DESKTOP=1 (boot straight into TDE like a real install).
    if [[ -n ${ROOTDISK:-} ]]; then
        # Boot an installed system: no initramfs, "/" from the disk labelled SYLUXROOT.
        printf 'timeout: 0\n\n/Syluxium (disk root)\n    protocol: limine\n    path: boot():/boot/syluxium.elf\n    cmdline: root=LABEL=SYLUXROOT %s\n' \
            "${ROOTCMD:-}" > "$OUT/limine-test.conf"
        mcopy -o -i "$DISK@@1M" "$OUT/limine-test.conf" ::/boot/limine/limine.conf
    elif [[ -n ${KCMD:-} ]]; then
        # The text console entry with extra kernel options.
        printf 'timeout: 0\n\n/Syluxium (test)\n    protocol: limine\n    path: boot():/boot/syluxium.elf\n    cmdline: %s\n    module_path: boot():/boot/initramfs.cpio\n' \
            "${DESKTOP:+desktop }$KCMD" > "$OUT/limine-test.conf"
        mcopy -o -i "$DISK@@1M" "$OUT/limine-test.conf" ::/boot/limine/limine.conf
    elif [[ ${DESKTOP:-0} != 1 ]]; then
        sed 's/^default_entry: 1$/default_entry: 2/' limine.conf > "$OUT/limine-test.conf"
        mcopy -o -i "$DISK@@1M" "$OUT/limine-test.conf" ::/boot/limine/limine.conf
    fi
fi

# VM size: VM_RAM (default 8G) and VM_CPUS (default 8); e.g. VM_RAM=2G to test a small machine.
# Hardware virtualization when available (10-50x faster boots); KVM=0 forces
# software emulation (TCG), useful to rule out KVM-specific behaviour.
if [[ ${KVM:-1} != 0 && -r /dev/kvm && -w /dev/kvm ]]; then
    ACCEL="-accel kvm -cpu host"
else
    ACCEL="-cpu max"
fi
args=(-M q35 -m ${VM_RAM:-8G} -smp ${VM_CPUS:-8} $ACCEL
      -drive "file=$DISK,format=raw,if=ide"
      ${ROOTDISK:+-drive "file=$ROOTDISK,format=raw,if=ide,index=3"}
      -display none -serial "file:$LOG"
      -monitor "unix:$SOCK,server,nowait"
      -no-reboot)
# QEMU's "edu" teaching device: the LinuxKPI test driver (kernel/linuxkpi/drivers/edu.c) binds to it.
args+=(-device edu,dma_mask=0xffffffffffffffff)
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

# Wait for the shell prompt (or give up after <seconds>): with KVM the system
# is usually ready long before the limit.
waited=0
# READY=<text>: wait for that instead (e.g. a login screen, no shell prompt).
until grep -q "${READY:-root@syluxium}" "$LOG" 2>/dev/null; do
    sleep 0.5
    waited=$((waited + 1))
    if (( waited >= SECONDS_TO_RUN * 2 )) || ! kill -0 $QEMU_PID 2>/dev/null; then
        break
    fi
done
sleep 1

# Optionally type on the keyboard, then take a screenshot and quit.
python3 - "$SOCK" "$PPM" "${TYPE:-}" "$LOG" <<'EOF'
import os, socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
names = {" ": "spc", "\n": "ret", "-": "minus", "=": "equal", ".": "dot", ",": "comma", "/": "slash",
         ";": "semicolon", "'": "apostrophe", "[": "bracket_left", "]": "bracket_right", "\\": "backslash",
         "`": "grave_accent"}
shifted = {"|": "backslash", "_": "minus", "+": "equal", ">": "dot", "<": "comma", "?": "slash", ":": "semicolon",
           '"': "apostrophe", "{": "bracket_left", "}": "bracket_right", "~": "grave_accent", "!": "1", "@": "2",
           "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0"}
text = sys.argv[3].replace("\\n", "\n").replace("\\c", "\x03").replace("\\w", "\x01").replace("\\m", "\x02").replace("\\s", "\x04").replace("\\o", "\x05").replace("\\t", "\x06").replace("\\e", "\x07")
# "\{monitor command}" and "\[serial text]": pulled out first, executed in order with the keys.
import re as _re
pieces = []
for tok in _re.split(r"(\\\{[^}]*\}|\\\[[^\]]*\])", text):
    if tok.startswith("\\{") and tok.endswith("}"):
        pieces.append(("monitor", tok[2:-1]))
    elif tok.startswith("\\[") and tok.endswith("]"):
        pieces.append(("wait", tok[2:-1]))
    else:
        pieces += list(tok)
for ch in pieces:
    if isinstance(ch, tuple) and ch[0] == "wait":  # \[text]: wait for it on the serial log
        end = time.time() + float(os.environ.get("MARK_WAIT", "180"))
        while time.time() < end:
            try:
                if ch[1].encode() in open(sys.argv[4], "rb").read():
                    break
            except OSError:
                pass
            time.sleep(0.25)
        else:
            print(f"smoke-test: no \"{ch[1]}\" on the serial log", file=sys.stderr)
        continue
    if isinstance(ch, tuple):  # \{cmd}: its reply goes to monitor-<mode>.log
        s.sendall((ch[1] + "\n").encode()); time.sleep(1)
        s.setblocking(False)
        try:
            reply = s.recv(65536).decode(errors="replace")
        except BlockingIOError:
            reply = ""
        s.setblocking(True)
        with open(sys.argv[2].rsplit("/", 1)[0] + "/monitor-" + sys.argv[2].rsplit("-", 1)[-1].split(".")[0] + ".log", "a") as m:
            import re
            clean = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", reply).replace("\r", "")
            lines = [l for l in clean.split("\n") if l.strip() and not l.startswith("(qemu)") and ch[1] not in l]
            m.write(f">>> {ch[1]}\n" + "\n".join(lines) + "\n")
        continue
    if ch == "\x03":       # \c in $TYPE: Ctrl+C
        s.sendall(b"sendkey ctrl-c\n"); time.sleep(0.3); continue
    if ch == "\x06":       # \t in $TYPE: Ctrl+Alt+Del
        s.sendall(b"sendkey ctrl-alt-delete\n"); time.sleep(0.5); continue
    if ch == "\x07":       # \e in $TYPE: Ctrl+Esc (Xfce start menu)
        s.sendall(b"sendkey ctrl-esc\n"); time.sleep(0.5); continue
    if ch == "\x05":       # \o in $TYPE: Alt+F1 (launcher menu)
        s.sendall(b"sendkey alt-f1\n"); time.sleep(0.5); continue
    if ch == "\x04":       # \s in $TYPE: Alt+SysRq+W
        s.sendall(b"sendkey alt-sysrq-w\n"); time.sleep(0.5); continue
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
    # Done when the serial output has been quiet for 3 s (commands finished),
    # at most 10 minutes.
    log, last, quiet_since, start = sys.argv[4], -1, time.time(), time.time()
    while time.time() - start < 600:
        size = os.path.getsize(log) if os.path.exists(log) else 0
        if size != last:
            last, quiet_since = size, time.time()
        elif time.time() - quiet_since >= float(os.environ.get("QUIET", "3")):
            break
        time.sleep(0.25)
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
