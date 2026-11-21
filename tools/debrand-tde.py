#!/usr/bin/env python3
"""Menu and launcher names (.desktop/.directory files) without TDE's
branding: run on the assembled root file system at build time."""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from syluxium_names import rename

KEY = re.compile(r"^(Name|GenericName|Comment)(\[en[^\]]*\])?=(.*)$")

# Programs Syluxium lists under its own names (usr/share/applications/
# syluxium-*.desktop, e.g. Accessories > Calculator): their own entries are
# hidden so they don't show up twice.
HIDE = {"kcalc.desktop", "kolourpaint.desktop", "KCharSelect.desktop", "ksnapshot.desktop", "abiword.desktop"}


def main(root):
    changed = 0
    for top in ("opt/trinity/share", "usr/share/applications"):
        for dirpath, _, files in os.walk(os.path.join(root, top)):
            for name in files:
                if not name.endswith((".desktop", ".directory")):
                    continue
                path = os.path.join(dirpath, name)
                if os.path.islink(path):
                    continue
                try:
                    lines = open(path, encoding="utf-8").read().split("\n")
                except (UnicodeDecodeError, OSError):
                    continue
                new = []
                for line in lines:
                    m = KEY.match(line)
                    r = rename(m.group(3)) if m else None
                    new.append(f"{m.group(1)}{m.group(2) or ''}={r}" if r else line)
                    if line == "[Desktop Entry]" and name in HIDE and "/applications" in dirpath:
                        new.append("NoDisplay=true")
                if new != lines:
                    with open(path, "w", encoding="utf-8") as f:
                        f.write("\n".join(new))
                    changed += 1
    print(f"  NAMES {changed} menu entries renamed")


if __name__ == "__main__":
    main(sys.argv[1])
