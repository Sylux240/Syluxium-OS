# Sourced by the notification-area bubbles (Wi-Fi, AutoPlay): run their
# kdialog windows in the Dusk bubble colours (glass navy, light text, cyan).
# TDE takes colours from $TDEHOME/share/config/kdeglobals over the system
# file, so the bubbles get a small settings folder of their own; fonts and
# everything else still come from /etc/trinity/kdeglobals.
balloon_home="${HOME:-/tmp}/.syluxium-balloon"
if ! grep -q "Dusk" "$balloon_home/share/config/kdeglobals" 2>/dev/null; then
    mkdir -p "$balloon_home/share/config"
    cat > "$balloon_home/share/config/kdeglobals" <<'CFG'
# Dusk bubble colours
[General]
background=18,23,54
foreground=238,242,255
windowBackground=20,26,58
windowForeground=238,242,255
buttonBackground=37,45,92
buttonForeground=238,242,255
selectBackground=111,227,240
selectForeground=20,26,58
alternateBackground=24,31,66
linkColor=111,227,240
CFG
fi
# Same icons, styles and caches as the session, only the colours differ.
for d in cache-$(hostname) tmp-$(hostname) socket-$(hostname); do
    [ -e "$HOME/.trinity/$d" ] && [ ! -e "$balloon_home/$d" ] && ln -s "$HOME/.trinity/$d" "$balloon_home/$d"
done
balloon() { TDEHOME="$balloon_home" kdialog "$@"; }
