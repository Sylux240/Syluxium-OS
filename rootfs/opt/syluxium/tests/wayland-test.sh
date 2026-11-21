#!/bin/bash
# Bring-up test for the Wayland shell: seatd + Wayfire on the console's
# display, then report what happened (output goes to the console/serial).
export XDG_RUNTIME_DIR=/run/user/0
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
seatd -g video > /tmp/seatd.log 2>&1 &
sleep 1
export LIBSEAT_BACKEND=seatd
[ -n "$WLR_DRM_DEVICES" ] || export WLR_DRM_DEVICES=$(ls /dev/dri/card* | head -1)
echo "=== wayland-test: DRM device $WLR_DRM_DEVICES"; ls -la /sys/class/drm/ 2>&1 | head; cat /sys/class/drm/card0/uevent 2>&1
wayfire -c "${1:-/etc/syluxium-shell/wayfire.ini}" > /tmp/wf.log 2>&1 &
WF=$!
sleep "${2:-15}"
echo "=== wayland-test: wayfire $(kill -0 $WF 2>/dev/null && echo running || echo exited)"
ls -la "$XDG_RUNTIME_DIR"
echo "=== seatd log"; tail -5 /tmp/seatd.log
echo "=== wayfire log"; grep -v "^$" /tmp/wf.log | tail -40
