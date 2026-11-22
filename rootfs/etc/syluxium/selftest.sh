#!/bin/sh
# Phase 4b self-test: exercises fork/exec/wait, pipes, signals, scripts and
# the dynamic loader using real Debian programs (busybox ash, bash, GNU hello).
# Prints one PASS/FAIL line per check; exit status = number of failures.

pass=0
fail=0
check() {
    # check "description" command...
    desc=$1
    shift
    if "$@" >/tmp/selftest.out 2>&1; then
        echo "  [PASS] $desc"
        pass=$((pass + 1))
    else
        echo "  [FAIL] $desc"
        tail -n 6 /tmp/selftest.out | sed 's/^/         /'
        fail=$((fail + 1))
    fi
}

echo "selftest: running as pid $$ in $(busybox | head -n 1 | cut -d' ' -f1-2)"

# --- Dynamic linking: Debian binaries loading Debian's glibc ---
check "GNU hello from Debian's .deb"        sh -c '[ "$(hello)" = "Hello, world!" ]'
check "bash runs (dynamically linked)"      sh -c '[ "$(bash -c "echo \$((6 * 7))")" = 42 ]'
check "bash arrays and [[ ]]"               bash -c 'a=(x y z); [[ ${#a[@]} -eq 3 && ${a[1]} == y ]]'

# --- fork / exec / wait ---
check "command substitution"                sh -c '[ "$(echo nested $(echo deep))" = "nested deep" ]'
check "exit status through wait()"          sh -c '(exit 7); [ $? -eq 7 ]'
check "50 fork+exec+wait in a row"          sh -c 'i=0; while [ $i -lt 50 ]; do /bin/true || exit 1; i=$((i+1)); done'
check "background job + wait"               sh -c 'sleep 0.2 & wait $!'
check "static binary exec'd from a shell"   /opt/syluxium/tests/hello

# --- pipes ---
check "pipe: echo | tr | grep"              sh -c 'echo hello | tr a-z A-Z | grep -q HELLO'
check "pipe: 1000 lines through wc"         sh -c '[ "$(seq 1 1000 | wc -l)" -eq 1000 ]'
check "pipe: 1 MiB through cat"             sh -c '[ "$(head -c 1048576 /dev/zero | cat | wc -c)" -eq 1048576 ]'

# --- files and redirection ---
check "redirect > and >>"                   sh -c 'echo one > /tmp/r; echo two >> /tmp/r; [ "$(cat /tmp/r)" = "one
two" ]'
check "shebang script"                      sh -c 'printf "#!/bin/sh\necho script-ok\n" > /tmp/s.sh && chmod +x /tmp/s.sh && [ "$(/tmp/s.sh)" = script-ok ]'
check "ls /usr/bin finds programs"          sh -c '[ "$(ls /usr/bin | wc -l)" -gt 100 ]'

# --- signals ---
check "trap a signal (handler + sigreturn)" sh -c 'trap "echo caught" USR1; kill -USR1 $$; true'
check "signal handler output"               sh -c '[ "$(sh -c "trap \"echo caught\" USR1; kill -USR1 \$\$; echo after")" = "caught
after" ]'
check "kill a sleeping process (SIGTERM)"   sh -c 'sleep 10 & p=$!; kill $p; wait $p; [ $? -eq 143 ]'
check "SIGPIPE when the reader is gone"     sh -c 'yes | head -n 3 >/dev/null'

# --- /proc and /sys ---
check "/proc/self/exe points at the program" sh -c 'readlink /proc/self/exe | grep -q -e readlink -e busybox'
check "/proc/cpuinfo lists every CPU"       sh -c '[ "$(grep -c ^processor /proc/cpuinfo)" -eq "$(nproc)" ]'
check "/proc/meminfo has MemTotal"          sh -c 'grep -q "^MemTotal:" /proc/meminfo'
check "ps sees this shell"                  sh -c 'ps -A -o args | grep -q selftest'
check "/proc/<pid>/cmdline"                 sh -c 'sleep 3 & p=$!; tr "\\0" " " < /proc/$p/cmdline | grep -q "^sleep 3"; r=$?; kill $p; exit $r'
check "/proc/<pid>/fd lists open files"     sh -c 'exec 7</etc/hostname; [ "$(readlink /proc/$$/fd/7)" = /etc/hostname ]'
check "free / uptime work"                  sh -c 'free >/dev/null && uptime >/dev/null'
check "/sys/devices/system/cpu/online"      sh -c 'grep -q "^0" /sys/devices/system/cpu/online'
# GPU buffers shared between programs (dma-buf): its own mapping and the
# DRM mapping are the same memory.
[ -e /dev/dri/card0 ] && check "dma-buf mmap (PRIME export)"   python3 -c 'import os, fcntl, struct, mmap
fd = os.open("/dev/dri/card0", os.O_RDWR)
c = bytearray(fcntl.ioctl(fd, 0xC02064B2, struct.pack("IIIIIIQ", 64, 64, 32, 0, 0, 0, 0)))
h, pitch, size = struct.unpack("IIIIIIQ", c)[4:]
off = struct.unpack("IIQ", bytearray(fcntl.ioctl(fd, 0xC01064B3, struct.pack("IIQ", h, 0, 0))))[2]
dfd = struct.unpack("IIi", bytearray(fcntl.ioctl(fd, 0xC00C642D, struct.pack("IIi", h, 0x80002, -1))))[2]
a = mmap.mmap(dfd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
b = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=off)
a[100:104] = b"Slx!"; assert b[100:104] == b"Slx!", b[100:104]'
# mincore: Mesa's EGL checks with it that a display pointer is real memory
# (every EGL program failed without it). Touched page in memory, untouched
# one not, an unmapped range ENOMEM.
check "mincore (Mesa's EGL pointer check)"  python3 -c 'import ctypes, errno
libc = ctypes.CDLL(None, use_errno=True)
libc.mmap.restype = ctypes.c_void_p
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_long]
a = libc.mmap(None, 3 * 4096, 3, 0x22, -1, 0)
ctypes.memset(a, 1, 1)
v = (ctypes.c_ubyte * 3)()
assert libc.mincore(ctypes.c_void_p(a), 3 * 4096, v) == 0, ctypes.get_errno()
assert v[0] == 1 and v[2] == 0, list(v)
libc.munmap(ctypes.c_void_p(a), 3 * 4096)
assert libc.mincore(ctypes.c_void_p(a), 4096, v) == -1 and ctypes.get_errno() == errno.ENOMEM, ctypes.get_errno()'
# Punched holes in a memfd (fallocate PUNCH_HOLE|KEEP_SIZE; foot's scrolling
# buffer): zeros through read() and through a shared mapping made before,
# the edges kept, the page usable again.
check "memfd hole punching (fallocate)"     python3 -c 'import ctypes, mmap, os
libc = ctypes.CDLL(None, use_errno=True)
fd = os.memfd_create("punch")
os.write(fd, b"A" * 4096 + b"B" * 8192 + b"C" * 4096)
m = mmap.mmap(fd, 4 * 4096, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
assert m[5000] == ord("B")
r = libc.fallocate(fd, 3, ctypes.c_long(100), ctypes.c_long(3 * 4096))
assert r == 0, ctypes.get_errno()
assert m[99] == ord("A") and m[100] == 0 and m[5000] == 0 and m[12387] == 0 and m[12388] == ord("C"), (m[99], m[100], m[5000], m[12388])
assert os.pread(fd, 4096, 4096) == bytes(4096)
assert os.fstat(fd).st_size == 4 * 4096
m[6000] = 7
assert os.pread(fd, 1, 6000) == b"\x07"'
# Terminal emulators on VTE (xfce4-terminal) read the pty in packet mode.
# (In a session of its own: closing the master hangs up the slave's group.)
check "pty packet mode (TIOCPKT)"           setsid python3 -c 'import os, fcntl, struct
m, s = os.openpty(); fcntl.ioctl(m, 0x5420, struct.pack("i", 1)); os.write(s, b"hi")
d = os.read(m, 16); assert d == b"\0hi", d'

# --- threads and pipes under stress (1 s each part): futex handoffs (a
# lost wakeup hangs it: the program's own watchdog fails it), both pipe ends
# closed at once, dup storms, fork/exec with inherited pipes ---
check "futex stress (GIL handoff, condvars, timeouts)" /opt/syluxium/tests/futexstress 1
check "pipe / fd stress (close races, dup, fork)"     /opt/syluxium/tests/pipestress 1
check "futex wakeups while forking, WAKE_OP, PI, robust" /opt/syluxium/tests/futexfork 1
check "copy_file_range, pidfd, sigtimedwait, mlock"   /opt/syluxium/tests/newsyscalls

echo "selftest: $pass passed, $fail failed"
exit $fail
