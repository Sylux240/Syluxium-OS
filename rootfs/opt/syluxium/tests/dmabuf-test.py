#!/usr/bin/python3
"""Create a DRM dumb buffer, export it as a dma-buf (PRIME), mmap the dma-buf
fd, write and read back: the path Mesa's llvmpipe uses to import buffers."""
import fcntl, mmap, os, struct, sys
card = sys.argv[1] if len(sys.argv) > 1 else "/dev/dri/card0"
fd = os.open(card, os.O_RDWR)
def IOWR(nr, size): return (3 << 30) | (size << 16) | (ord('d') << 8) | nr
# DRM_IOCTL_MODE_CREATE_DUMB: height, width, bpp, flags, handle, pitch, size
b = bytearray(struct.pack("IIIIIIQ", 64, 64, 32, 0, 0, 0, 0))
fcntl.ioctl(fd, IOWR(0xB2, 32), b)
h, w, bpp, flags, handle, pitch, size = struct.unpack("IIIIIIQ", b)
print("dumb handle", handle, "pitch", pitch, "size", size)
# DRM_IOCTL_PRIME_HANDLE_TO_FD: handle, flags (O_RDWR|O_CLOEXEC), fd
p = bytearray(struct.pack("Iii", handle, os.O_RDWR | os.O_CLOEXEC, -1))
fcntl.ioctl(fd, IOWR(0x2D, 12), p)
dfd = struct.unpack("Iii", p)[2]
print("dma-buf fd", dfd, os.readlink(f"/proc/self/fd/{dfd}"))
m = mmap.mmap(dfd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
m[0:4] = b"\x11\x22\x33\x44"
print("mmap ok, read back", m[0:4].hex())
# and import it again (PRIME_FD_TO_HANDLE)
q = bytearray(struct.pack("Iii", 0, 0, dfd))
fcntl.ioctl(fd, IOWR(0x2E, 12), q)
print("re-import handle", struct.unpack("Iii", q)[0])
