#!/bin/sh
# Phase 4d self-test: unmodified 32-bit (i386) Debian programs, dynamically
# linked against Debian's i386 glibc (/usr/lib/i386-linux-gnu, ld-linux.so.2).
# Prints one PASS/FAIL line per check; exit status = number of failures.

pass=0
fail=0
check() {
    desc=$1
    shift
    if "$@" >/dev/null 2>&1; then
        echo "  [PASS] $desc"
        pass=$((pass + 1))
    else
        echo "  [FAIL] $desc"
        fail=$((fail + 1))
    fi
}

echo "selftest-ia32: $(hello-i386)"
check "GNU hello (i386, dynamic)"            sh -c '[ "$(hello-i386)" = "Hello, world!" ]'
check "i386 busybox echo"                    sh -c '[ "$(busybox-i386 echo ok32)" = ok32 ]'
check "i386 busybox ls + wc"                 sh -c '[ "$(busybox-i386 ls /usr/bin | busybox-i386 wc -l)" -gt 100 ]'
check "i386 busybox sh: fork/exec/pipes"     busybox-i386 sh -c 'x=$(echo a b c | busybox-i386 tr " " "\n" | busybox-i386 wc -l); [ $x -eq 3 ]'
check "i386 busybox sh: traps (signals)"     busybox-i386 sh -c 'trap "echo caught" USR1; kill -USR1 $$; true'
check "i386 bash arithmetic + arrays"        bash-i386 -c 'a=(1 2 3); [ $((a[0] + a[2])) -eq 4 ]'
check "i386 bash runs a 64-bit program"      bash-i386 -c '[ "$(hello)" = "Hello, world!" ]'
check "64-bit bash runs i386 bash"           bash -c '[ "$(bash-i386 -c "echo \$((6 * 7))")" = 42 ]'
check "i386 cat/stat of a file"              sh -c 'echo data > /tmp/f32; [ "$(busybox-i386 cat /tmp/f32)" = data ] && busybox-i386 stat /tmp/f32'
check "i386 date / sleep (time syscalls)"    sh -c 'busybox-i386 date >/dev/null && busybox-i386 sleep 0.1'

echo "selftest-ia32: $pass passed, $fail failed"
exit $fail
