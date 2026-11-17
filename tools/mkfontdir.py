#!/usr/bin/env python3
"""Write fonts.dir for a directory of X11 PCF fonts (what Debian's font
packages do in their install scripts with mkfontdir, which Syluxium doesn't
run yet). Each line: file name and the font's XLFD name, read from the PCF
"FONT" property."""
import gzip
import os
import struct
import sys

PCF_PROPERTIES = 1


def pcf_font_name(data: bytes):
    if data[:4] != b"\x01fcp":
        return None
    (count,) = struct.unpack_from("<I", data, 4)
    for i in range(count):
        ttype, fmt, size, offset = struct.unpack_from("<IIII", data, 8 + i * 16)
        if ttype != PCF_PROPERTIES:
            continue
        (tfmt,) = struct.unpack_from("<I", data, offset)
        end = ">" if tfmt & 4 else "<"  # PCF_BYTE_MASK: most significant byte first
        (nprops,) = struct.unpack_from(end + "I", data, offset + 4)
        props = []
        pos = offset + 8
        for _ in range(nprops):
            name_off, is_string, value = struct.unpack_from(end + "iBi", data, pos)
            props.append((name_off, is_string, value))
            pos += 9
        if nprops & 3:
            pos += 4 - (nprops & 3)
        (string_size,) = struct.unpack_from(end + "I", data, pos)
        strings = data[pos + 4 : pos + 4 + string_size]

        def cstr(o):
            return strings[o : strings.index(b"\0", o)].decode("latin-1")

        for name_off, is_string, value in props:
            if cstr(name_off) == "FONT" and is_string:
                return cstr(value)
    return None


def main(directory: str) -> None:
    entries = []
    for name in sorted(os.listdir(directory)):
        path = os.path.join(directory, name)
        if not (name.endswith(".pcf") or name.endswith(".pcf.gz")):
            continue
        raw = open(path, "rb").read()
        if name.endswith(".gz"):
            raw = gzip.decompress(raw)
        xlfd = pcf_font_name(raw)
        if xlfd:
            entries.append(f"{name} {xlfd.lower()}")
    with open(os.path.join(directory, "fonts.dir"), "w") as f:
        f.write(f"{len(entries)}\n")
        f.write("\n".join(entries) + "\n")
    print(f"fonts.dir: {len(entries)} fonts in {directory}")


if __name__ == "__main__":
    for d in sys.argv[1:]:
        if os.path.isdir(d):
            main(d)
