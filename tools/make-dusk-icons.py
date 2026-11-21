#!/usr/bin/env python3
"""Syluxium's Dusk icons: line drawings in the logo's cyan (pink for the
trash, gold for documents) on a rounded glass tile, as in the Dusk mockup.
Only the icons the desktop and launcher show most; everything else comes
from the theme this one inherits (TDE's crystalsvg).

Writes <rootfs>/opt/trinity/share/icons/Syluxium/<size>x<size>/apps/<name>.png
and index.theme. Usage: make-dusk-icons.py [rootfs]"""
import math
import os
import sys

from PIL import Image, ImageDraw

SS = 8
SIZES = (16, 22, 32, 48, 64, 128)
GLASS = (18, 23, 54)
CYAN = (111, 227, 240)
PINK = (242, 154, 182)
GOLD = (246, 201, 107)
LAV = (185, 163, 227)


class Pen:
    """Draws in a 24x24 design grid (like the mockup's SVG icons)."""

    def __init__(self, img, box, color, width=1.8):
        self.g = ImageDraw.Draw(img)
        self.x0, self.y0, self.s = box[0], box[1], box[2] / 24
        self.c = color + (255,)
        self.w = max(1, round(width * self.s))

    def p(self, x, y):
        return (self.x0 + x * self.s, self.y0 + y * self.s)

    def line(self, *pts):
        self.g.line([self.p(*q) for q in pts], fill=self.c, width=self.w, joint="curve")
        for q in (pts[0], pts[-1]):  # round caps
            x, y = self.p(*q)
            r = self.w / 2
            self.g.ellipse((x - r, y - r, x + r, y + r), fill=self.c)

    def rect(self, x0, y0, x1, y1, r=2, fill=None):
        self.g.rounded_rectangle((*self.p(x0, y0), *self.p(x1, y1)), r * self.s, outline=self.c, width=self.w,
                                 fill=fill)

    def circle(self, cx, cy, r, fill=None):
        x, y = self.p(cx, cy)
        rr = r * self.s
        self.g.ellipse((x - rr, y - rr, x + rr, y + rr), outline=self.c, width=self.w,
                       fill=self.c if fill is True else fill)

    def arc(self, cx, cy, rx, ry, a0, a1):
        x, y = self.p(cx, cy)
        self.g.arc((x - rx * self.s, y - ry * self.s, x + rx * self.s, y + ry * self.s), a0, a1, fill=self.c,
                   width=self.w)

    def poly(self, pts, fill):
        self.g.polygon([self.p(*q) for q in pts], fill=fill)


def folder(p, color):
    p.poly([(3, 7), (9, 7), (11, 9), (21, 9), (21, 19), (3, 19)], fill=color + (40,))
    p.line((3, 17), (3, 7), (5, 5), (9, 5), (11, 7), (19, 7), (21, 9), (21, 17), (19, 19), (5, 19), (3, 17))


def draw(name, p):
    if name in ("computer", "syluxium-system"):
        p.rect(3, 4, 21, 16, 2)
        p.line((8, 20), (16, 20))
        p.line((12, 16), (12, 20))
    elif name in ("folder", "folder_home", "folder_wordprocessing", "syluxium-files", "folder_open"):
        folder(p, CYAN)
    elif name == "folder_image":
        folder(p, PINK)
    elif name in ("network", "syluxium-network"):
        p.circle(12, 5, 2)
        p.circle(5, 19, 2)
        p.circle(19, 19, 2)
        p.line((12, 7), (12, 12))
        p.line((12, 12), (6.5, 17.5))
        p.line((12, 12), (17.5, 17.5))
    elif name in ("trashcan_empty", "trashcan_full", "user-trash", "user-trash-full"):
        p.line((4, 7), (20, 7))
        p.line((9, 7), (9, 4), (15, 4), (15, 7))
        p.line((6, 7), (7, 20), (17, 20), (18, 7))
        if "full" in name:
            p.line((10, 11), (10, 16))
            p.line((14, 11), (14, 16))
    elif name in ("syluxium-web", "web-browser", "konqueror"):
        p.circle(12, 12, 9)
        p.line((3, 12), (21, 12))
        p.arc(12, 12, 4.5, 9, 0, 360)
    elif name in ("system-software-install", "syluxium-installer"):
        p.line((12, 3), (12, 14))
        p.line((7.5, 9.5), (12, 14), (16.5, 9.5))
        p.line((4, 16), (4, 20), (20, 20), (20, 16))
    elif name in ("system-software-update",):
        p.arc(12, 12, 8, 8, 20, 320)
        p.line((20, 4), (20, 9), (15, 9))
    elif name in ("kcontrol", "preferences-system", "syluxium-settings"):
        p.circle(12, 12, 3)
        for a in range(0, 360, 45):
            c, s = math.cos(math.radians(a)), math.sin(math.radians(a))
            p.line((12 + 6.5 * c, 12 + 6.5 * s), (12 + 9.5 * c, 12 + 9.5 * s))
    elif name in ("konsole", "utilities-terminal"):
        p.rect(3, 4, 21, 20, 2)
        p.line((7, 9), (10, 12), (7, 15))
        p.line((13, 15), (17, 15))
    elif name in ("media-flash", "drive-removable-media", "usbpendrive_unmount"):
        p.rect(8, 9, 16, 21, 2)
        p.rect(9.5, 3, 14.5, 9, 1)
    elif name in ("printer",):
        p.rect(4, 9, 20, 17, 2)
        p.line((7, 9), (7, 4), (17, 4), (17, 9))
        p.rect(7, 14, 17, 20, 1)
    # Accessories
    elif name in ("syluxium-calculator", "kcalc", "accessories-calculator"):
        p.rect(5, 3, 19, 21, 2)
        p.rect(8, 6, 16, 9.5, 1)
        for y in (13.5, 17.5):
            for x in (9, 12, 15):
                p.circle(x, y, 0.6, fill=True)
    elif name in ("syluxium-paint", "kolourpaint"):
        p.arc(12, 12, 9, 9, 30, 330)
        p.line((19.8, 7.5), (16.5, 9.8), (16.5, 14.2), (19.8, 16.5))
        for x, y in ((8, 9), (12.5, 6.8), (7.5, 14), (11.5, 17.2)):
            p.circle(x, y, 1.2, fill=True)
    elif name in ("syluxium-charmap", "kcharselect", "accessories-character-map"):
        p.rect(3, 4, 21, 20, 2)
        p.line((6.5, 16.5), (10, 7.5), (13.5, 16.5))
        p.line((7.8, 13.2), (12.2, 13.2))
        for x, y in ((17, 8.5), (17, 12), (17, 15.5)):
            p.circle(x, y, 0.7, fill=True)
    elif name in ("syluxium-snip", "ksnapshot", "applets-screenshooter"):
        p.line((3, 8), (3, 3), (8, 3))
        p.line((16, 3), (21, 3), (21, 8))
        p.line((21, 16), (21, 21), (16, 21))
        p.line((8, 21), (3, 21), (3, 16))
        p.circle(12, 12, 3.5)
        p.line((12, 6.5), (12, 8.5))
        p.line((12, 15.5), (12, 17.5))
        p.line((6.5, 12), (8.5, 12))
        p.line((15.5, 12), (17.5, 12))
    elif name in ("syluxium-wordpad", "abiword"):
        p.line((15, 21), (6, 21), (4, 19), (4, 5), (6, 3), (14, 3), (18, 7), (18, 11))
        p.line((7.5, 8), (13, 8))
        p.line((7.5, 12), (14, 12))
        p.line((7.5, 16), (11, 16))
        p.line((21, 12.5), (14.5, 19), (13.5, 21.5), (16, 20.5), (22, 14.5))
    elif name in ("syluxium-network-connections", "network-wired"):
        p.rect(2.5, 4, 11.5, 11, 1.5)
        p.rect(12.5, 13, 21.5, 20, 1.5)
        p.line((7, 11), (7, 16.5), (12.5, 16.5))
        p.line((17, 13), (17, 7.5), (11.5, 7.5))
    elif name in ("syluxium-bluetooth", "bluetooth", "preferences-system-bluetooth"):
        p.line((6.5, 7.5), (17, 16.5), (12, 21), (12, 3), (17, 7.5), (6.5, 16.5))
    elif name in ("syluxium-notification", "preferences-desktop-notification"):
        p.line((6, 17), (6, 11))
        p.arc(12, 11, 6, 6, 180, 360)
        p.line((18, 11), (18, 17))
        p.line((4, 17), (20, 17))
        p.line((10.5, 20), (13.5, 20))


ICONS = {
    "computer": CYAN, "syluxium-system": CYAN, "folder": CYAN, "folder_home": CYAN, "folder_open": CYAN,
    "folder_wordprocessing": GOLD, "syluxium-files": CYAN, "folder_image": PINK, "network": CYAN,
    "syluxium-network": CYAN, "trashcan_empty": PINK, "trashcan_full": PINK, "user-trash": PINK,
    "user-trash-full": PINK, "syluxium-web": CYAN, "system-software-install": CYAN, "syluxium-installer": CYAN,
    "system-software-update": CYAN, "kcontrol": CYAN, "syluxium-settings": CYAN, "konsole": CYAN,
    "utilities-terminal": CYAN, "media-flash": CYAN, "printer": LAV,
    # Accessories (and the programs behind them, so their windows match)
    "syluxium-calculator": CYAN, "kcalc": CYAN, "accessories-calculator": CYAN,
    "syluxium-paint": PINK, "kolourpaint": PINK,
    "syluxium-charmap": LAV, "kcharselect": LAV, "accessories-character-map": LAV,
    "syluxium-snip": CYAN, "ksnapshot": CYAN, "applets-screenshooter": CYAN,
    "syluxium-wordpad": GOLD, "abiword": GOLD,
    "syluxium-network-connections": CYAN, "network-wired": CYAN,
    "syluxium-notification": GOLD, "preferences-desktop-notification": GOLD,
    "syluxium-bluetooth": CYAN, "bluetooth": CYAN, "preferences-system-bluetooth": CYAN,
}
COLOR_OVERRIDE = {"folder_wordprocessing": CYAN}  # documents folder: cyan like the mockup's folders


def icon(name, size):
    S = size * SS
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    g = ImageDraw.Draw(im)
    tile = size >= 32  # small sizes: just the drawing, larger: on the glass tile
    color = COLOR_OVERRIDE.get(name, ICONS[name])
    if tile:
        m = S * 0.04
        g.rounded_rectangle((m, m, S - m, S - m), S * 0.26, fill=GLASS + (215,),
                            outline=tuple(int(GLASS[i] + (color[i] - GLASS[i]) * 0.40) for i in range(3)) + (255,),
                            width=max(SS, int(S * 0.02)))
        inner = S * 0.56
        box = ((S - inner) / 2, (S - inner) / 2, inner)
        width = 1.8
    else:
        box = (0, 0, S)
        width = 2.0 if size >= 22 else 2.2
    draw(name, Pen(im, box, color, width))
    return im.resize((size, size), Image.LANCZOS)


def main(root):
    base = os.path.join(root, "opt/trinity/share/icons/Syluxium")
    dirs = []
    for size in SIZES:
        d = os.path.join(base, f"{size}x{size}/apps")
        os.makedirs(d, exist_ok=True)
        dirs.append(f"{size}x{size}/apps")
        for name in ICONS:
            icon(name, size).save(os.path.join(d, name + ".png"))
    with open(os.path.join(base, "index.theme"), "w") as f:
        f.write("[Icon Theme]\nName=Syluxium Dusk\nComment=Syluxium's Dusk icons (tools/make-dusk-icons.py)\n")
        f.write("Inherits=crystalsvg,Adwaita,hicolor\nDisplayDepth=32\n")
        f.write("Directories=" + ",".join(dirs) + "\n\n")
        for size, d in zip(SIZES, dirs):
            f.write(f"[{d}]\nSize={size}\nContext=Applications\nType=Fixed\n\n")
    print(f"{len(ICONS)} icons x {len(SIZES)} sizes written to {base}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "rootfs")
