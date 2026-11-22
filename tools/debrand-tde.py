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
HIDE = {"kcalc.desktop", "kolourpaint.desktop", "KCharSelect.desktop", "ksnapshot.desktop", "abiword.desktop",
        "konsole.desktop",
        # the GTK programs behind them (Calculator, Paint, Character Map,
        # Snipping Tool, Code Editor, Terminal; Thunar stays: a Start menu favourite)
        "galculator.desktop", "com.github.maoschanz.drawing.desktop", "gucharmap.desktop",
        "xfce4-screenshooter.desktop", "org.xfce.mousepad.desktop", "xfce4-terminal.desktop",
        "yad-icon-browser.desktop"}


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
    window_icons(root)


ICON_SIZES = ("16x16", "22x22", "32x32", "48x48", "64x64", "128x128")


def window_icons(root):
    """Window icons for Trinity's programs in the Wayland shell. There an X11
    window is known to the taskbar and Task Manager only by its WM_CLASS
    ("Kcalc"), which they look up as a .desktop file (by id, then by
    searching names and commands); Trinity's own entries live outside the
    XDG data folders. Hidden entries (no menu, no MIME types: never in a
    start menu or "Open With") carry each program's name, command and icon,
    and its icon is linked into hicolor when no installed theme has it."""
    src = os.path.join(root, "opt/trinity/share/applications/tde")
    dst = os.path.join(root, "usr/share/applications")
    if not os.path.isdir(src):
        return
    have = set()
    for t in ("Syluxium", "hicolor", "Adwaita"):
        for _, _, files in os.walk(os.path.join(root, "usr/share/icons", t)):
            have.update(f.rsplit(".", 1)[0] for f in files)
    tde_icons = [os.path.join(root, "opt/trinity/share/icons", t) for t in ("hicolor", "crystalsvg")]
    made = linked = 0
    for name in sorted(os.listdir(src)):
        if not name.endswith(".desktop"):
            continue
        try:
            text = open(os.path.join(src, name), encoding="utf-8").read()
        except (UnicodeDecodeError, OSError):
            continue
        keys = dict(re.findall(r"^(Name|Exec|Icon|Type)=(.*)$", text, re.M))
        if keys.get("Type") != "Application" or not keys.get("Exec") or \
                re.search(r"tdecmshell|X-TDE-ModuleType|KCModule", text):
            continue
        icon = keys.get("Icon", "")
        if icon and not icon.startswith("/") and icon not in have:
            for size in ICON_SIZES:
                for t in tde_icons:
                    f = os.path.join(t, size, "apps", icon + ".png")
                    if os.path.exists(f):
                        d = os.path.join(root, "usr/share/icons/hicolor", size, "apps")
                        os.makedirs(d, exist_ok=True)
                        link = os.path.join(d, icon + ".png")
                        if not os.path.lexists(link):
                            os.symlink(os.path.relpath(f, d), link)  # relative: the icon cache is built outside the system
                            linked += 1
                        break
        # GLib drops entries whose program isn't on $PATH (/opt/trinity/bin isn't).
        exe = keys["Exec"].split()[0]
        if not exe.startswith("/") and os.path.exists(os.path.join(root, "opt/trinity/bin", exe)):
            keys["Exec"] = "/opt/trinity/bin/" + keys["Exec"]
        base = name[:-len(".desktop")].lower()
        with open(os.path.join(dst, f"trinity-{base}.desktop"), "w", encoding="utf-8") as f:
            f.write("[Desktop Entry]\n# Window icon for the Wayland shell (tools/debrand-tde.py)\n"
                    f"Type=Application\nName={keys.get('Name', base)}\nExec={keys['Exec']}\n"
                    f"Icon={icon}\nNoDisplay=true\n")
        made += 1
    # Debian entries naming their icon as a file ("Icon=sopwith.png"): the
    # shell's taskbar (libxfce4windowing) looks the name up in the icon theme
    # as it is and finds nothing; menus strip the extension themselves.
    fixed = 0
    for name in os.listdir(dst):
        path = os.path.join(dst, name)
        if not name.endswith(".desktop") or os.path.islink(path):
            continue
        try:
            text = open(path, encoding="utf-8").read()
        except (UnicodeDecodeError, OSError):
            continue
        new = re.sub(r"^(Icon=)([^/\n]+)\.(png|xpm|svg)$", r"\1\2", text, flags=re.M)
        if new != text:
            with open(path, "w", encoding="utf-8") as f:
                f.write(new)
            fixed += 1
        # ... and only searches icon themes: an icon only in /usr/share/pixmaps
        # is linked into hicolor.
        m = re.search(r"^Icon=([^/\n]+)$", new, re.M)
        if m and m.group(1) not in have:
            for ext, size in (("svg", "scalable"), ("png", "48x48")):
                f = os.path.join(root, "usr/share/pixmaps", f"{m.group(1)}.{ext}")
                if os.path.exists(f):
                    d = os.path.join(root, "usr/share/icons/hicolor", size, "apps")
                    os.makedirs(d, exist_ok=True)
                    if not os.path.lexists(os.path.join(d, f"{m.group(1)}.{ext}")):
                        os.symlink(os.path.relpath(f, d), os.path.join(d, f"{m.group(1)}.{ext}"))
                        linked += 1
                    have.add(m.group(1))
                    break
    print(f"  ICONS {made} Trinity window icons for the Wayland shell, {fixed} icon names without a file "
          f"extension, {linked} icons linked into hicolor")



if __name__ == "__main__":
    main(sys.argv[1])
