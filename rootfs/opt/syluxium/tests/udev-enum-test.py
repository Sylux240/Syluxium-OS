#!/usr/bin/env python3
# What libudev's enumeration finds in /sys (desktop hardware libraries such
# as TDE's tdehwlib enumerate everything this way, without a udev daemon):
#   udev-enum-test.py [subsystem]   (prints syspath, subsystem and a property)
# Also opens one device directly, reporting errno on failure.
import ctypes
import os
import sys

u = ctypes.CDLL("libudev.so.1", use_errno=True)
u.udev_new.restype = ctypes.c_void_p
u.udev_enumerate_new.restype = ctypes.c_void_p
u.udev_enumerate_new.argtypes = [ctypes.c_void_p]
u.udev_enumerate_add_match_subsystem.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
u.udev_enumerate_scan_devices.argtypes = [ctypes.c_void_p]
u.udev_enumerate_scan_devices.restype = ctypes.c_int
u.udev_enumerate_get_list_entry.restype = ctypes.c_void_p
u.udev_enumerate_get_list_entry.argtypes = [ctypes.c_void_p]
u.udev_list_entry_get_next.restype = ctypes.c_void_p
u.udev_list_entry_get_next.argtypes = [ctypes.c_void_p]
u.udev_list_entry_get_name.restype = ctypes.c_char_p
u.udev_list_entry_get_name.argtypes = [ctypes.c_void_p]
u.udev_device_new_from_syspath.restype = ctypes.c_void_p
u.udev_device_new_from_syspath.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
u.udev_device_get_subsystem.restype = ctypes.c_char_p
u.udev_device_get_subsystem.argtypes = [ctypes.c_void_p]
u.udev_device_get_property_value.restype = ctypes.c_char_p
u.udev_device_get_property_value.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

udev = u.udev_new()
for p in (b"/sys/class/power_supply/BAT0", b"/sys/bus/pci/devices/0000:00:00.0", b"/sys/class/sound/card0"):
    ctypes.set_errno(0)
    d = u.udev_device_new_from_syspath(udev, p)
    err = ctypes.get_errno()
    print("open", p.decode(), "->", "ok" if d else f"failed: {os.strerror(err)} ({err})",
          (u.udev_device_get_subsystem(d) or b"-").decode() if d else "")

e = u.udev_enumerate_new(udev)
want = sys.argv[1].encode() if len(sys.argv) > 1 else None
if want:
    u.udev_enumerate_add_match_subsystem(e, want)
r = u.udev_enumerate_scan_devices(e)
print("scan:", r)
entry = u.udev_enumerate_get_list_entry(e)
n = 0
while entry:
    path = u.udev_list_entry_get_name(entry)
    dev = u.udev_device_new_from_syspath(udev, path)
    sub = u.udev_device_get_subsystem(dev) if dev else None
    name = u.udev_device_get_property_value(dev, b"POWER_SUPPLY_NAME") if dev else None
    if n < 60:
        print(path.decode(), sub.decode() if sub else "-", name.decode() if name else "")
    n += 1
    entry = u.udev_list_entry_get_next(entry)
print("devices:", n)
