#!/usr/bin/env python3
"""/dev/uhid: a keyboard made by this program, as BlueZ makes Bluetooth ones.

Creates a HID keyboard (boot-keyboard report descriptor, Bluetooth bus),
waits for UHID_START, then types "abc" + Enter with input reports.
  uhid-test.py [seconds to stay before destroying the device]
"""
import os
import struct
import sys
import time

UHID_DESTROY, UHID_START, UHID_CREATE2, UHID_INPUT2 = 1, 2, 11, 12
EVENT_SIZE = 4380
BUS_BLUETOOTH = 0x05

# A boot keyboard: modifiers, reserved, 6 key codes; 5 LED bits out.
DESCRIPTOR = bytes([
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
])


def event(kind, payload=b""):
    return (struct.pack("<I", kind) + payload).ljust(EVENT_SIZE, b"\0")


def main():
    stay = float(sys.argv[1]) if len(sys.argv) > 1 else 5
    fd = os.open("/dev/uhid", os.O_RDWR)
    create = struct.pack("<128s64s64sHHIIII", b"Syluxium Test Bluetooth Keyboard", b"", b"",
                         len(DESCRIPTOR), BUS_BLUETOOTH, 0x05AC, 0x0255, 1, 0) + DESCRIPTOR
    os.write(fd, event(UHID_CREATE2, create))
    ev = os.read(fd, EVENT_SIZE)
    print("uhid: first event", struct.unpack("<I", ev[:4])[0], "(2 = UHID_START)", flush=True)
    time.sleep(1)
    for usage in (0x04, 0x05, 0x06, 0x28):  # a, b, c, Enter
        for report in (bytes([0, 0, usage, 0, 0, 0, 0, 0]), bytes(8)):
            os.write(fd, event(UHID_INPUT2, struct.pack("<H", len(report)) + report))
            time.sleep(0.05)
    print("uhid: typed abc + Enter", flush=True)
    time.sleep(stay)
    os.write(fd, event(UHID_DESTROY))
    os.close(fd)
    print("uhid: destroyed", flush=True)


if __name__ == "__main__":
    main()
