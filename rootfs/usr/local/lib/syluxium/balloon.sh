# Sourced by the notification-area bubbles (Wi-Fi, AutoPlay): run their
# kdialog windows with XP balloon colours (pale yellow, black text).
# TDE takes colours from $TDEHOME/share/config/kdeglobals over the system
# file, so the bubbles get a small settings folder of their own; fonts and
# everything else still come from /etc/trinity/kdeglobals.
balloon_home="${HOME:-/tmp}/.syluxium-balloon"
if [ ! -f "$balloon_home/share/config/kdeglobals" ]; then
    mkdir -p "$balloon_home/share/config"
    cat > "$balloon_home/share/config/kdeglobals" <<'CFG'
[General]
background=255,255,225
foreground=0,0,0
windowBackground=255,255,225
windowForeground=0,0,0
buttonBackground=255,255,225
buttonForeground=0,0,0
selectBackground=49,106,197
selectForeground=255,255,255
alternateBackground=250,250,215
linkColor=0,0,255
CFG
fi
# Same icons, styles and caches as the session, only the colours differ.
for d in cache-$(hostname) tmp-$(hostname) socket-$(hostname); do
    [ -e "$HOME/.trinity/$d" ] && [ ! -e "$balloon_home/$d" ] && ln -s "$HOME/.trinity/$d" "$balloon_home/$d"
done
balloon() { TDEHOME="$balloon_home" kdialog "$@"; }
