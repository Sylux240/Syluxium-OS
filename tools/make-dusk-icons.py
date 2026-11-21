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


ICONS = {
    "computer": CYAN, "syluxium-system": CYAN, "folder": CYAN, "folder_home": CYAN, "folder_open": CYAN,
    "folder_wordprocessing": GOLD, "syluxium-files": CYAN, "folder_image": PINK, "network": CYAN,
    "syluxium-network": CYAN, "trashcan_empty": PINK, "trashcan_full": PINK, "user-trash": PINK,
    "user-trash-full": PINK, "syluxium-web": CYAN, "system-software-install": CYAN, "syluxium-installer": CYAN,
    "system-software-update": CYAN, "kcontrol": CYAN, "syluxium-settings": CYAN, "konsole": CYAN,
    "utilities-terminal": CYAN, "media-flash": CYAN, "printer": LAV,
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
        f.write("Inherits=crystalsvg,hicolor\nDisplayDepth=32\n")
        f.write("Directories=" + ",".join(dirs) + "\n\n")
        for size, d in zip(SIZES, dirs):
            f.write(f"[{d}]\nSize={size}\nContext=Applications\nType=Fixed\n\n")
    print(f"{len(ICONS)} icons x {len(SIZES)} sizes written to {base}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "rootfs")
