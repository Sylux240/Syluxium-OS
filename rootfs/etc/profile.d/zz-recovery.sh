# The "Recovery" boot entry (boot option "recovery"): the recovery menu on
# the first console instead of the desktop, once per boot.
# (/dev/console under Syluxium's own kernel, /dev/tty1 under Linux's init)
case "$(tty 2>/dev/null)" in /dev/console|/dev/tty1) first_console=1 ;; *) first_console= ;; esac
if [ -n "$first_console" ] && [ ! -e /run/recovery-started ] &&
   grep -qw recovery /proc/cmdline 2>/dev/null && [ -x /usr/local/sbin/syluxium-recovery ]; then
    : > /run/recovery-started
    /usr/local/sbin/syluxium-recovery
fi
