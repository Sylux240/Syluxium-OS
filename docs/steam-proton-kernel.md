# What Steam, Proton and pressure-vessel need from the kernel

What the Steam client (32-bit, plus the Chromium-based `steamwebhelper`),
the Steam Linux Runtime's container tool `pressure-vessel` (bubblewrap
inside) and Proton (Wine + DXVK/VKD3D) ask of the kernel, and what
Syluxium's kernel has. Status: **done**, **partial**, **missing**.

## Threads, waiting, signals

| Feature | Who uses it | Status |
|---|---|---|
| futex (WAIT/WAKE, BITSET, CMP_REQUEUE as wake-all) | everything (glibc) | done |
| futex_waitv (syscall 449) | Proton's fsync (`WINEFSYNC=1`, the default) | done: up to 128 futexes, timeouts on MONOTONIC/REALTIME; tested in `futexstress` |
| futex PI (LOCK_PI etc.) | glibc PI mutexes (rare) | missing (ENOSYS) |
| clone3 | glibc 2.34+ pthread_create, bwrap | partial: everything clone() can do; CLONE_PIDFD, set_tid, cgroups answer ENOSYS (glibc falls back) |
| signals lost by a thread about to sleep | anything signal-driven | fixed (wait conditions re-checked once asleep) |
| rt_sigtimedwait / sigwaitinfo | dbus, xcape, glibc timers | done |
| POSIX timers (timer_create & co.) | coreutils `timeout`, glibc SIGEV_THREAD timers, games | done: signal, thread-directed, SIGEV_THREAD (via glibc), si_value delivered |
| timerfd, eventfd, signalfd, epoll (incl. epoll_pwait2) | Wine, Steam, GLib | done; edge cases (TFD_TIMER_CANCEL_ON_SET, eventfd semaphore + O_NONBLOCK races) not audited |
| rseq | glibc 2.35+ | quietly ENOSYS (glibc copes) |
| membarrier | .NET, some engines | done |
| sched_setaffinity / getaffinity | Proton, games | done |

## Processes

| Feature | Who uses it | Status |
|---|---|---|
| pidfd_open, pidfd_send_signal | dbus, pressure-vessel, Steam's reaper | done (poll for exit) |
| pidfd_getfd (438), waitid(P_PIDFD) | systemd-style tools | missing |
| close_range (incl. CLOEXEC) | bwrap, glib spawn | done |
| process_vm_readv / writev | Wine (ReadProcessMemory), debuggers | done (same user or root) |
| ptrace | Steam's crash reporter (breakpad), gdb, strace, some anti-cheat | **missing** |
| prctl | bwrap, Steam | partial: PR_SET_NAME, PDEATHSIG (accepted but not acted on), NO_NEW_PRIVS (accepted, not enforced/remembered), CHILD_SUBREAPER (accepted, not acted on), capability queries |
| personality(ADDR_NO_RANDOMIZE) | `setarch -R`, Wine's preloader | done |
| 32-bit (ia32) system calls | the Steam client is 32-bit | done (compat layer) |

## Sandboxing and containers

| Feature | Who uses it | Status |
|---|---|---|
| user namespaces, uid_map/gid_map | pressure-vessel (bwrap), Flatpak | partial (Android agent's work) |
| mount namespaces, bind mounts, pivot_root | pressure-vessel | partial |
| PID / UTS / IPC / net namespaces | bwrap | partial |
| seccomp (classic BPF filters, syscall 317) | steamwebhelper (Chromium sandbox), bwrap `--seccomp`, Flatpak | **missing**: prctl(PR_SET_SECCOMP) reports success without filtering, which a sandbox would trust; should become a real cBPF interpreter or an honest EINVAL |
| Landlock | newer sandboxes | missing (they fall back) |
| FUSE (/dev/fuse) | AppImages, some launchers; not pressure-vessel | **missing** |

## Files

| Feature | Who uses it | Status |
|---|---|---|
| openat2 | newer runtimes | partial: no RESOLVE_* (asking for one gives ENOSYS, so callers fall back) |
| copy_file_range | cp, install, Steam's file copies | done |
| statx, faccessat2 | glibc | done |
| memfd_create + seals | Wine, Vulkan drivers, Steam | done |
| inotify | Steam (library folders), Wine | done |
| files deleted while open (ext4) | everything (temp files, game updates) | fixed: kept until the last close; orphan list for power cuts |
| io_uring | some games, newer Wine versions (optional) | **missing** (callers fall back to read/write/epoll) |
| fallocate, O_DIRECT | Steam downloads | not audited |

## Devices

| Feature | Who uses it | Status |
|---|---|---|
| /dev/uinput | Steam Input (virtual controllers) | **missing** |
| /dev/hidraw* | Steam Controller, DualShock/DualSense, Switch Pro | **missing** |
| evdev joysticks (/dev/input/event*) | games via SDL | done (evdev), controller drivers depend on hardware |
| DRM render nodes, Vulkan | DXVK, VKD3D | GPU work in progress (other agent) |

## Cheapest next steps

1. **seccomp-bpf** (classic BPF interpreter at syscall entry, per-thread filter
   chains inherited by fork/clone, RET_ALLOW/ERRNO/TRAP(SIGSYS)/KILL/LOG):
   unblocks Chromium's sandbox (steamwebhelper) and makes `PR_SET_SECCOMP` honest.
2. **uinput** and **hidraw**: Steam Input and most controllers.
3. **ptrace** (at least PTRACE_ATTACH/PEEK/POKE/GETREGS/CONT/DETACH and
   PTRACE_TRACEME): crash reporting, gdb, strace.
4. NO_NEW_PRIVS and PDEATHSIG for real (bwrap relies on both).
5. io_uring and FUSE are larger and can wait: everything that uses them has a
   fallback except FUSE-based AppImages.
