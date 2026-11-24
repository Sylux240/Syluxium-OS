# Firefox's sandboxes need seccomp and namespaces: the Linux kernel has them;
# Syluxium's own kernel doesn't, so there Firefox runs its processes without.
if [ ! -e /proc/sys/kernel/seccomp/actions_avail ]; then
    export MOZ_DISABLE_CONTENT_SANDBOX=1 MOZ_DISABLE_GMP_SANDBOX=1 MOZ_DISABLE_RDD_SANDBOX=1 \
           MOZ_DISABLE_SOCKET_PROCESS_SANDBOX=1 MOZ_DISABLE_UTILITY_SANDBOX=1
fi
# GTK programs (Firefox): the window manager draws the title bars.
export GTK_CSD=0
