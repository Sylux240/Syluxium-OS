#!/bin/sh
# The system-wide jobs Debian's package install scripts and triggers run
# after installing packages, done once at image build time with Debian's
# own tools from the image (run through the image's dynamic loader):
# MIME database, image loaders, GTK input methods, GLib settings schemas,
# icon caches.
# Usage: run-triggers.sh <rootfs>
R=$(realpath "$1")
LIBS="$R/lib/x86_64-linux-gnu:$R/usr/lib/x86_64-linux-gnu"
run() { "$R/lib64/ld-linux-x86-64.so.2" --library-path "$LIBS" "$@"; }
n=0
if [ -x "$R/usr/bin/update-mime-database" ]; then
    run "$R/usr/bin/update-mime-database" "$R/usr/share/mime" > /dev/null 2>&1 && n=$((n + 1))
fi
Q="$R/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/gdk-pixbuf-query-loaders"
D="$R/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0"
if [ -x "$Q" ]; then
    # Paths in the cache must be the ones the running system sees.
    run "$Q" "$D"/loaders/*.so 2>/dev/null | sed "s|$R/|/|g" > "$D/loaders.cache" && n=$((n + 1))
fi
# GTK input method modules (fcitx5's): their cache lists them.
G="$R/usr/lib/x86_64-linux-gnu/libgtk-3-0t64/gtk-query-immodules-3.0"
M="$R/usr/lib/x86_64-linux-gnu/gtk-3.0/3.0.0"
if [ -x "$G" ] && [ -d "$M/immodules" ]; then
    run "$G" "$M"/immodules/*.so 2>/dev/null | sed "s|$R/|/|g" > "$M/immodules.cache" && n=$((n + 1))
fi
S="$R/usr/lib/x86_64-linux-gnu/glib-2.0/glib-compile-schemas"
if [ -x "$S" ] && [ -d "$R/usr/share/glib-2.0/schemas" ]; then
    run "$S" "$R/usr/share/glib-2.0/schemas" && n=$((n + 1))
fi
for theme in "$R"/usr/share/icons/*/; do
    [ -f "$theme/index.theme" ] && run "$R/usr/bin/gtk-update-icon-cache" -q -f -t "$theme" 2>/dev/null && n=$((n + 1))
done
echo "  TRIG  $n system caches built (MIME, image loaders, input methods, schemas, icons)"
