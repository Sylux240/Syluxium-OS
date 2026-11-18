# Boot straight into the desktop: the default boot menu entry passes
# "desktop" on the kernel command line. Only the first console login of a
# boot does this; logging out of TDE leaves you at this text shell.
if [ "$(tty 2>/dev/null)" = /dev/console ] && [ ! -e /run/desktop-started ] &&
   grep -qw desktop /proc/cmdline 2>/dev/null && [ -x /usr/local/bin/startdesktop ]; then
    : > /run/desktop-started
    echo "Starting the login screen..."
    /usr/local/bin/startdesktop
fi
