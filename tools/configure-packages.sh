#!/bin/sh
# Runs Debian's package install scripts once, at image build time:
# `dpkg --configure -a` over the fetched root file system (packages were
# recorded "unpacked" by debian-fetch.py --dpkg-db), in a user namespace
# (fake root with all user IDs mapped, no sudo) with /proc and /dev.
# Services are not started (policy-rc.d). Files the scripts gave to other
# users are listed in /var/lib/syluxium/ownership (applied at boot, since
# the image is packed as root), then everything is handed back to root so
# the build can copy it. Setuid/setgid modes are kept (and listed in
# /var/lib/syluxium/modes, since changing a file's owner clears them).
# Usage: configure-packages.sh <rootfs> <log>
set -e
R=$(realpath "$1")
LOG=$(realpath -m "$2")
mkdir -p "$R/proc" "$R/dev" "$R/tmp" "$R/run" "$R/var/lib/syluxium"
# The basic commands come from busybox (as in the image: the Makefile makes
# the same links), so the install scripts find sh, sed, grep, ...
for applet in $("$R/usr/bin/busybox" --list); do
    [ -e "$R/usr/bin/$applet" ] || [ -L "$R/usr/bin/$applet" ] || [ -e "$R/usr/sbin/$applet" ] || ln -s busybox "$R/usr/bin/$applet"
done
[ -e "$R/usr/bin/sh" ] || ln -s busybox "$R/usr/bin/sh"
# Accounts first (as debootstrap does), so scripts can chown to root, lp, ...
for f in passwd group; do
    [ -s "$R/etc/$f" ] || cp "$R/usr/share/base-passwd/$f.master" "$R/etc/$f"
done
# Manual-page folders (the fetcher leaves the pages out; update-alternatives
# still links into them).
for n in 1 2 3 4 5 6 7 8; do mkdir -p "$R/usr/share/man/man$n"; done
cat > "$R/usr/sbin/policy-rc.d" <<'POL'
#!/bin/sh
exit 101
POL
chmod 755 "$R/usr/sbin/policy-rc.d"
unshare --map-root-user --map-auto --mount --pid --fork sh -c '
    R=$1
    mount -t proc proc "$R/proc"
    mount --rbind /dev "$R/dev"
    chroot "$R" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root LC_ALL=C.UTF-8 \
        DEBIAN_FRONTEND=noninteractive DEBCONF_NONINTERACTIVE_SEEN=true \
        /usr/bin/dpkg --force-confdef --force-confold --force-depends --configure -a
    status=$?
    chroot "$R" /usr/bin/dpkg --audit > /tmp/audit.txt 2>&1 || true
    # Who owns what (other than root), then everything back to root.
    chroot "$R" /usr/bin/python3 -c "
import os
import stat
out = open(\"/var/lib/syluxium/ownership\", \"w\")
modes = open(\"/var/lib/syluxium/modes\", \"w\")
for top in (\"/etc\", \"/var\", \"/usr\", \"/opt\", \"/srv\", \"/home\"):
    for d, dirs, files in os.walk(top):
        for n in dirs + files:
            p = os.path.join(d, n)
            st = os.lstat(p)
            if st.st_uid or st.st_gid:
                out.write(f\"{st.st_uid} {st.st_gid} {p}\n\")
            if not stat.S_ISLNK(st.st_mode) and st.st_mode & 0o6000:
                modes.write(f\"{stat.S_IMODE(st.st_mode):o} {p}\n\")
"
    for d in etc var usr opt srv home root; do [ -e "$R/$d" ] && chown -hR 0:0 "$R/$d"; done
    # chown clears the setuid/setgid bits (sudo, passwd, su, pkexec, ...): put them back.
    while read -r mode path; do chmod "$mode" "$R$path"; done < "$R/var/lib/syluxium/modes"
    umount -l "$R/dev" "$R/proc" 2>/dev/null || true
    exit $status
' sh "$R" > "$LOG" 2>&1 || echo "  warning: some install scripts failed (see $LOG)"
rm -f "$R/usr/sbin/policy-rc.d"
unconfigured=$(grep -c "^Status: install ok \(unpacked\|half-configured\|half-installed\)" "$R/var/lib/dpkg/status" || true)
configured=$(grep -c "^Status: install ok installed" "$R/var/lib/dpkg/status" || true)
owned=$(wc -l < "$R/var/lib/syluxium/ownership")
echo "  DPKG  $configured packages configured, $unconfigured not; $owned files owned by system users"
