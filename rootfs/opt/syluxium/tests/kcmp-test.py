#!/usr/bin/python3
"""kcmp: the same open file through dup() compares equal, a separate open() doesn't; flock works."""
import ctypes, fcntl, os
libc = ctypes.CDLL(None, use_errno=True)
fd1 = os.open("/etc/hostname", os.O_RDONLY)
fd2 = os.dup(fd1)
fd3 = os.open("/etc/hostname", os.O_RDONLY)
pid = os.getpid()
same = libc.syscall(312, pid, pid, 0, fd1, fd2)
diff = libc.syscall(312, pid, pid, 0, fd1, fd3)
fcntl.flock(fd1, fcntl.LOCK_EX); fcntl.flock(fd1, fcntl.LOCK_UN)
print("kcmp same:", "PASS" if same == 0 else f"FAIL {same}", "different:", "PASS" if diff in (1, 2) else f"FAIL {diff}", "flock: PASS")
