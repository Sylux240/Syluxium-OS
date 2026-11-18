# Every graphical session gets its own D-Bus session bus (TDE talks to it).
# Sourced by TDM's Xsession: re-run the whole session inside dbus-run-session.
if [ -z "$DBUS_SESSION_BUS_ADDRESS" ] && [ -z "$SYLUXIUM_DBUS_WRAPPED" ]; then
    export SYLUXIUM_DBUS_WRAPPED=1
    exec dbus-run-session -- "$0" "$@"
fi
