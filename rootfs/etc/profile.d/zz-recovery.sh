# The "Recovery" boot entry (boot option "recovery"): the recovery menu on
# the console instead of the desktop, once per boot.
if [ "$(tty 2>/dev/null)" = /dev/console ] && [ ! -e /run/recovery-started ] &&
   grep -qw recovery /proc/cmdline 2>/dev/null && [ -x /usr/local/sbin/syluxium-recovery ]; then
    : > /run/recovery-started
    /usr/local/sbin/syluxium-recovery
fi
