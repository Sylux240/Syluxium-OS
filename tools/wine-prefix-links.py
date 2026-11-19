#!/usr/bin/env python3
"""Shrink a freshly created Wine prefix by turning its DLL copies into symlinks.

    wine-prefix-links.py ROOTFS PREFIX

A new Wine prefix holds a full copy of every builtin DLL and program
(C:\\windows\\system32, syswow64): about 500 MB, all byte-identical to the
files in /usr/lib/x86_64-linux-gnu/wine/<arch>-windows. Syluxium's live root
is in RAM, so the template prefix shipped in the image links to those files
(and the fonts/NLS tables from /usr/share/wine) instead, as Proton does. PREFIX is the prefix directory inside ROOTFS; the
links are absolute paths as seen from inside the image.

The prefix's .update-timestamp is set to "disable": a Wine prefix update
rewrites the fake DLLs in place, which through these links would truncate
the real ones (the DLLs are links, so new Wine versions reach every prefix
without an update anyway).
"""
import hashlib
import os
import sys

WINE_DIR = "usr/lib/x86_64-linux-gnu/wine"


def digest(path: str) -> str:
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def main() -> None:
    rootfs, prefix = sys.argv[1], sys.argv[2]
    by_content = {}  # (size, sha256) -> path inside the image
    # The DLLs/programs, and the data files (fonts, NLS tables) copied from /usr/share/wine.
    for d in (os.path.join(WINE_DIR, "x86_64-windows"), os.path.join(WINE_DIR, "i386-windows"), "usr/share/wine"):
        for dirpath, _, files in os.walk(os.path.join(rootfs, d)):
            for name in files:
                p = os.path.join(dirpath, name)
                if os.path.isfile(p) and not os.path.islink(p):
                    by_content.setdefault((os.path.getsize(p), digest(p)), "/" + os.path.relpath(p, rootfs))
    sizes = {k[0] for k in by_content}

    linked = saved = 0
    for dirpath, _, files in os.walk(os.path.join(prefix, "drive_c")):
        for name in files:
            p = os.path.join(dirpath, name)
            if os.path.islink(p):
                continue
            size = os.path.getsize(p)
            if size not in sizes:
                continue
            target = by_content.get((size, digest(p)))
            if target:
                os.remove(p)
                os.symlink(target, p)
                linked += 1
                saved += size
    with open(os.path.join(prefix, ".update-timestamp"), "w") as f:
        f.write("disable\n")
    print(f"  wine prefix: {linked} files linked to the builtin DLLs ({saved // 1048576} MiB saved)")


if __name__ == "__main__":
    main()
