#!/usr/bin/python3
"""mremap: grow with move, data kept; shrink; grow in place."""
import ctypes, mmap
libc = ctypes.CDLL(None, use_errno=True)
libc.mmap.restype = ctypes.c_void_p
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_long]
libc.mremap.restype = ctypes.c_void_p
libc.mremap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t, ctypes.c_int]
P = 4096
a = libc.mmap(None, 4 * P, 3, 0x22, -1, 0)
blocker = libc.mmap(a + 4 * P, P, 3, 0x22 | 0x10, -1, 0)  # MAP_FIXED right after: no room in place
ctypes.memmove(a, b"hello mremap" * 300, 12 * 300)
b = libc.mremap(a, 4 * P, 64 * P, 1)  # MAYMOVE
ok1 = b not in (None, ctypes.c_void_p(-1).value) and b != a and ctypes.string_at(b, 12) == b"hello mremap"
ctypes.memset(b + 63 * P, 7, P)  # the new tail is usable
c = libc.mremap(b, 64 * P, 8 * P, 0)  # shrink in place
ok2 = c == b and ctypes.string_at(c + 12 * 299, 12) == b"hello mremap"
d = libc.mremap(c, 8 * P, 16 * P, 0)  # grow in place (space after is free now)
ok3 = d == c
print("mremap move:", "PASS" if ok1 else "FAIL", "shrink:", "PASS" if ok2 else "FAIL", "grow in place:", "PASS" if ok3 else "FAIL")
