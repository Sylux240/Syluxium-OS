#!/bin/sh
# Pre-install an AppImage into the root file system:
#   tools/install-appimage.sh ROOTFS ID FILE
#
#   /opt/apps/ID/<file>.AppImage            the AppImage itself (mounted with FUSE when run)
#   /usr/local/bin/ID                       launcher (through syluxium-appimage: unpacked run
#                                           when FUSE isn't usable)
#   /usr/share/applications/ID.desktop      start menu entry, from the AppImage's own .desktop
#   /usr/share/icons/hicolor/*/apps/ID.*    its icon, taken out of the AppImage
#
# A missing FILE only gives a warning: the image builds without it. The
# AppImage's libraries are checked against the image's glibc, and a
# mismatch is reported (the program would not start).
set -u
ROOT=$1 ID=$2 FILE=$3
if [ -z "$FILE" ] || [ ! -f "$FILE" ]; then
    echo "  warning: AppImage for '$ID' not found (${FILE:-no path given}); not pre-installed"
    exit 0
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
NAME=$(basename "$FILE")
DEST=/opt/apps/$ID
mkdir -p "$ROOT$DEST" "$ROOT/usr/local/bin" "$ROOT/usr/share/applications"
rm -f "$ROOT$DEST"/*.AppImage
cp "$FILE" "$ROOT$DEST/$NAME"
chmod 755 "$ROOT$DEST/$NAME"

# The AppImage's top-level files (.desktop, icon): its own runtime unpacks
# them (no FUSE needed for --appimage-extract).
cp "$FILE" "$TMP/app" && chmod +x "$TMP/app"
(cd "$TMP" && ./app --appimage-extract >/dev/null 2>&1)
SQ=$TMP/squashfs-root
[ -d "$SQ" ] || echo "  warning: could not unpack $NAME (no menu entry details, no icon)"

desktop=$(ls "$SQ"/*.desktop 2>/dev/null | head -n 1)
icon_name=$ID
if [ -n "$desktop" ]; then
    icon_name=$(sed -n 's/^Icon=//p' "$desktop" | head -n 1)
    # Exec: the launcher (keeping the field codes); Icon: ours; no TryExec.
    sed -e "s|^Exec=[^ ]*|Exec=/usr/local/bin/$ID|" -e "s|^Icon=.*|Icon=$ID|" -e '/^TryExec=/d' \
        -e '/^X-AppImage-/d' "$desktop" > "$ROOT/usr/share/applications/$ID.desktop"
    printf 'X-Syluxium-AppImage=%s/%s\n' "$DEST" "$NAME" >> "$ROOT/usr/share/applications/$ID.desktop"
else
    printf '[Desktop Entry]\nType=Application\nName=%s\nExec=/usr/local/bin/%s %%F\nIcon=%s\nCategories=Utility;\nX-Syluxium-AppImage=%s/%s\n' \
        "$ID" "$ID" "$ID" "$DEST" "$NAME" > "$ROOT/usr/share/applications/$ID.desktop"
fi

# The icon: <Icon>.png/.svg at the top, else .DirIcon.
icon=
for c in "$SQ/$icon_name.svg" "$SQ/$icon_name.png" "$SQ/.DirIcon"; do
    [ -e "$c" ] && { icon=$(readlink -f "$c"); break; }
done
if [ -n "$icon" ] && [ -f "$icon" ]; then
    if head -c 512 "$icon" | grep -q "<svg"; then
        mkdir -p "$ROOT/usr/share/icons/hicolor/scalable/apps"
        cp "$icon" "$ROOT/usr/share/icons/hicolor/scalable/apps/$ID.svg"
    else
        # PNG width from its header, for the right hicolor folder.
        w=$(od -An -tu1 -j16 -N4 "$icon" | awk '{print $1*16777216+$2*65536+$3*256+$4}')
        case $w in 16|22|24|32|48|64|96|128|256|512) ;; *) w=256;; esac
        mkdir -p "$ROOT/usr/share/icons/hicolor/${w}x$w/apps"
        cp "$icon" "$ROOT/usr/share/icons/hicolor/${w}x$w/apps/$ID.png"
    fi
else
    echo "  warning: no icon found in $NAME"
fi

cat > "$ROOT/usr/local/bin/$ID" <<EOF
#!/bin/sh
# $ID: pre-installed AppImage ($DEST/$NAME), see tools/install-appimage.sh
exec /usr/local/bin/syluxium-appimage "$DEST/$NAME" "\$@"
EOF
chmod 755 "$ROOT/usr/local/bin/$ID"

# glibc: the newest symbol version the AppImage's own programs and libraries
# need, against the image's (Debian's) glibc.
if [ -d "$SQ" ] && command -v objdump >/dev/null; then
    need=$(find "$SQ" -type f \( -name '*.so*' -o -perm -u+x \) -exec objdump -T {} + 2>/dev/null |
           grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu | tail -n 1 | cut -d_ -f2)
    have=$(grep -aoE 'GLIBC_[0-9]+\.[0-9]+' "$ROOT/usr/lib/x86_64-linux-gnu/libc.so.6" 2>/dev/null |
           sort -Vu | tail -n 1 | cut -d_ -f2)
    if [ -n "$need" ] && [ -n "$have" ] &&
       [ "$(printf '%s\n%s\n' "$need" "$have" | sort -V | tail -n 1)" != "$have" ]; then
        echo "  warning: $NAME needs glibc $need, the image has $have: it will not start on Syluxium."
        echo "           Rebuild it on an older base (e.g. its own packaging/Dockerfile, Ubuntu 22.04)."
        echo "$NAME needs glibc $need (image: $have)" > "$ROOT$DEST/GLIBC-MISMATCH"
    fi
fi
echo "  APPIMAGE $ID: $DEST/$NAME"
