#!/usr/bin/env python3
"""Convert a binary PPM (P6, as written by QEMU's screendump) to PNG. No dependencies."""
import struct
import sys
import zlib


def read_token(data: bytes, pos: int) -> tuple[bytes, int]:
    while True:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos)
        else:
            break
    end = pos
    while not data[end:end + 1].isspace():
        end += 1
    return data[pos:end], end


def main() -> None:
    data = open(sys.argv[1], "rb").read()
    magic, pos = read_token(data, 0)
    if magic != b"P6":
        sys.exit("not a P6 PPM")
    width, pos = read_token(data, pos)
    height, pos = read_token(data, pos)
    maxval, pos = read_token(data, pos)
    width, height = int(width), int(height)
    if int(maxval) != 255:
        sys.exit("only 8-bit PPM supported")
    pixels = data[pos + 1:]

    stride = width * 3
    raw = b"".join(b"\x00" + pixels[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    open(sys.argv[2], "wb").write(png)


if __name__ == "__main__":
    main()
