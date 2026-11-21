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
    if "$@" >/dev/null 2>&1; then
        echo "  [PASS] $desc"
        pass=$((pass + 1))
    else
        echo "  [FAIL] $desc"
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

# --- threads and pipes under stress (1 s each part): futex handoffs (a
# lost wakeup hangs it: the program's own watchdog fails it), both pipe ends
# closed at once, dup storms, fork/exec with inherited pipes ---
check "futex stress (GIL handoff, condvars, timeouts)" /opt/syluxium/tests/futexstress 1
check "pipe / fd stress (close races, dup, fork)"     /opt/syluxium/tests/pipestress 1
check "copy_file_range, pidfd, sigtimedwait, mlock"   /opt/syluxium/tests/newsyscalls

echo "selftest: $pass passed, $fail failed"
exit $fail
