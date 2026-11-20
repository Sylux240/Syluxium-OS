#!/usr/bin/env python3
"""Syluxium's XP-era wording for a few TDE programs, as a small English
"translation" (language code en_XP, chosen in /etc/trinity/kdeglobals; any
string not listed here stays TDE's English). Writes gettext .mo catalogs to
<rootfs>/opt/trinity/share/locale/en_XP/LC_MESSAGES/<catalog>.mo.

TDE only uses a language for a program when that program's own catalog
exists in it, so programs without an entry here are unaffected."""
import os
import struct
import sys

LANG = "en_XP"

CATALOGS = {
    # TDE's common strings (used by the programs below): "3:46 PM"
    "tdelibs": {
        "am": "AM",
        "pm": "PM",
    },
    # Start menu (Kicker)
    "kicker": {
        "Run Command...": "Run...",
        "Log Out...": "Turn Off Computer...",
        "Lock Session": "Lock Computer",
        "TDE Menu": "Start Menu",
        "Switch User": "Switch User",
    },
    # Log off / turn off dialog (TDE session manager)
    "ksmserver": {
        'End Session for "%1"': "Turn off computer",
        "&Shutdown": "Turn &Off",
        "&Log out": "&Log Off",
        "Log out": "Log Off",
        "&Suspend": "Stand &By",
        "&Restart": "&Restart",
        "<qt><p>Log out of the current session to login as a different user.</p></qt>":
            "<qt><p>Close your programs and end your session.</p></qt>",
        "<qt><p>Log out of the current session and turn off the computer.</p></qt>":
            "<qt><p>Close your programs and turn off the computer.</p></qt>",
        "<qt><p>Log out of the current session and restart the computer.</p></qt>":
            "<qt><p>Close your programs and restart the computer.</p></qt>",
    },
    # Desktop: Run dialog, desktop menu
    "kdesktop": {
        "Run Command": "Run",
        "Run Command...": "Run...",
        "&Move to Trash": "&Delete",
    },
    # Control Panel (TDE Control Center)
    "kcontrol": {
        "Trinity Control Center": "Control Panel",
        "The Trinity Control Center": "Control Panel",
        "Welcome to the Trinity Control Center, a central place to configure your desktop environment. Select an item from the index on the left to load a configuration module.":
            "Welcome to Control Panel, where you change the settings of this computer. Pick a category on the left.",
    },
    # File manager (Konqueror)
    "konqueror": {
        "L&ocation: ": "A&ddress ",
        "&Move to Trash": "&Delete",
        "Trash": "Recycle Bin",
    },
}


def write_mo(path, messages):
    """A minimal GNU .mo file (sorted, no hash table)."""
    header = "Content-Type: text/plain; charset=UTF-8\nLanguage: en\n"
    entries = sorted([("", header)] + list(messages.items()))
    ids = b""
    strs = b""
    id_tab = []
    str_tab = []
    for k, v in entries:
        kb, vb = k.encode(), v.encode()
        id_tab.append((len(kb), len(ids)))
        ids += kb + b"\0"
        str_tab.append((len(vb), len(strs)))
        strs += vb + b"\0"
    n = len(entries)
    ids_off = 28 + 16 * n
    strs_off = ids_off + len(ids)
    out = struct.pack("<7I", 0x950412DE, 0, n, 28, 28 + 8 * n, 0, 0)
    out += b"".join(struct.pack("<2I", l, o + ids_off) for l, o in id_tab)
    out += b"".join(struct.pack("<2I", l, o + strs_off) for l, o in str_tab)
    out += ids + strs
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(out)


def main(root):
    d = os.path.join(root, "opt/trinity/share/locale", LANG, "LC_MESSAGES")
    for cat, msgs in CATALOGS.items():
        write_mo(os.path.join(d, cat + ".mo"), msgs)
    # TDE lists a language only if it has an entry.desktop
    with open(os.path.join(root, "opt/trinity/share/locale", LANG, "entry.desktop"), "w") as f:
        f.write("[KCM Locale]\nName=English (Syluxium)\n")
    print(f"catalogs written to {d}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "rootfs")
