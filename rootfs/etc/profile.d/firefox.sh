# Firefox's sandboxes need seccomp and namespaces, which the Syluxium
# kernel doesn't have yet: run its processes without them for now.
export MOZ_DISABLE_CONTENT_SANDBOX=1 MOZ_DISABLE_GMP_SANDBOX=1 MOZ_DISABLE_RDD_SANDBOX=1 \
       MOZ_DISABLE_SOCKET_PROCESS_SANDBOX=1 MOZ_DISABLE_UTILITY_SANDBOX=1
# GTK programs (Firefox): the window manager draws the title bars.
export GTK_CSD=0
