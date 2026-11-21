#!/usr/bin/env python3
"""Bluetooth without an adapter: a minimal controller behind /dev/vhci.

The kernel's virtual HCI driver turns what this program writes into a
Bluetooth controller (hci0); every HCI command the kernel sends is answered
with Command Complete (status 0, zero-filled parameters) except the few the
kernel needs real values for (version, address, buffer sizes).
  bt-vhci-test.py serve [seconds]   run the controller
  bt-vhci-test.py check             bring hci0 up through a raw HCI socket and print it
"""
import fcntl
import os
import socket
import struct
import sys
import time

ADDR = bytes([0x01, 0x02, 0x03, 0x04, 0xAA, 0x00])  # 00:AA:04:03:02:01 (little endian)


def reply(opcode, params):
    if opcode == 0x1001:  # Read Local Version Information
        return struct.pack("<BBHBHH", 0, 0x09, 0x0001, 0x09, 0x05F1, 0x0001)
    if opcode == 0x1009:  # Read BD_ADDR
        return b"\x00" + ADDR
    if opcode == 0x1005:  # Read Buffer Size
        return struct.pack("<BHBHH", 0, 1021, 64, 8, 8)
    if opcode == 0x1003:  # Read Local Supported Features (BR/EDR, no LE)
        return b"\x00" + bytes([0xFF, 0xFE, 0x0F, 0xFE, 0x18, 0x18, 0x00, 0x80])
    if opcode == 0x1004:  # Read Local Extended Features
        page = params[0] if params else 0
        return bytes([0, page, 1]) + bytes(8)
    if opcode == 0x0C14:  # Read Local Name
        return b"\x00" + b"Syluxium virtual controller".ljust(248, b"\x00")
    # Exact lengths where the kernel checks them: Reset, Read Class of
    # Device, Read Voice Setting, Read Number of Supported IAC.
    exact = {0x0C03: 1, 0x0C23: 4, 0x0C25: 3, 0x0C38: 2}
    if opcode in exact:
        return bytes(exact[opcode])
    return b"\x00" + bytes(64)


def serve(seconds):
    fd = os.open("/dev/vhci", os.O_RDWR)
    os.write(fd, bytes([0xFF, 0x00]))  # vendor packet: create a primary controller
    deadline = time.time() + seconds
    commands = 0
    while time.time() < deadline:
        pkt = os.read(fd, 4096)
        if not pkt:
            continue
        if pkt[0] == 0xFF:
            print("vhci: controller index", struct.unpack("<H", pkt[2:4])[0], flush=True)
            continue
        if pkt[0] != 0x01 or len(pkt) < 4:
            continue
        opcode = struct.unpack("<H", pkt[1:3])[0]
        params = pkt[4:4 + pkt[3]]
        rp = reply(opcode, params)
        body = struct.pack("<BH", 1, opcode) + rp
        os.write(fd, bytes([0x04, 0x0E, len(body)]) + body)
        commands += 1
    print("vhci: answered", commands, "commands", flush=True)


def check():
    HCIDEVUP = 0x400448C9
    HCIGETDEVINFO = 0x800448D3
    s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI)
    try:
        fcntl.ioctl(s, HCIDEVUP, 0)
    except OSError as e:
        print("HCIDEVUP:", e)
    # struct hci_dev_info: dev_id u16, name[8], bdaddr[6], flags u32, type u8, features[8], ...
    buf = bytearray(struct.pack("<H", 0) + bytes(90))
    fcntl.ioctl(s, HCIGETDEVINFO, buf)
    name = bytes(buf[2:10]).rstrip(b"\0").decode()
    addr = ":".join("%02X" % b for b in reversed(buf[10:16]))
    flags = struct.unpack("<I", bytes(buf[16:20]))[0]
    print("hci: %s %s flags %#x %s" % (name, addr, flags, "UP" if flags & 1 else "DOWN"))


if __name__ == "__main__":
    if sys.argv[1:2] == ["serve"]:
        serve(float(sys.argv[2]) if len(sys.argv) > 2 else 30)
    else:
        check()
