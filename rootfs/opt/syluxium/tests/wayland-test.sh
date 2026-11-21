#!/bin/bash
# Bring-up test for the Wayland shell: seatd (as root) hands the display and
# input devices to Wayfire running as the desktop user; then report.
U=${WAYLAND_USER:-syluxium}
UID_=$(id -u "$U")
RT=/run/user/$UID_
mkdir -p "$RT" && chown "$U" "$RT" && chmod 700 "$RT"
seatd -g video > /tmp/seatd.log 2>&1 &
sleep 1
chmod 666 /run/seatd.sock 2>/dev/null
su "$U" -s /bin/bash -c "export XDG_RUNTIME_DIR=$RT LIBSEAT_BACKEND=seatd WLR_RENDERER_ALLOW_SOFTWARE=1 EGL_LOG_LEVEL=debug MESA_DEBUG=1 LIBGL_DEBUG=verbose $WAYLAND_ENV; \
    exec wayfire -c ${1:-/etc/syluxium-shell/wayfire.ini}" > /tmp/wf.log 2>&1 &
sleep "${2:-20}"
echo "=== wayland-test: wayfire $(pgrep -x wayfire > /dev/null && echo running || echo exited)"
ls "$RT"
echo "=== seatd log"; tail -4 /tmp/seatd.log
echo "=== wayfire egl"; grep -aiE "egl|dri|gbm|renderer|swrast|allocator|dmabuf" /tmp/wf.log | head -40
echo "=== wayfire log"; grep -v "^$" /tmp/wf.log | grep -v addr2line | tail -30
