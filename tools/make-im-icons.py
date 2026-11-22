#!/usr/bin/env python3
"""The input method's taskbar icons in Dusk: the writing system's sign in
the logo's cyan on a rounded glass tile (fcitx5's own are red squares):
拼 Pinyin, あ Anthy, 한 Hangul, and a keyboard for plain typing.

Writes <rootfs>/opt/trinity/share/icons/Syluxium/<size>x<size>/apps/
{fcitx-pinyin,fcitx-anthy,fcitx-hangul,input-keyboard}.png (into the Dusk
icon theme, next to make-dusk-icons.py's).
Usage: make-im-icons.py ROOTFS CJK_FONT (a font with all three scripts,
e.g. WenQuanYi Micro Hei)"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

SS = 8
SIZES = (16, 22, 32, 48, 64, 128)
GLASS = (18, 23, 54)
CYAN = (111, 227, 240)
SIGNS = {"fcitx-pinyin": "拼", "fcitx-anthy": "あ", "fcitx-hangul": "한", "input-keyboard": None}


def keyboard(g, S, color):
    """A keyboard: outline and three rows of keys."""
    w = max(SS, int(S * 0.05))
    x0, y0, x1, y1 = S * 0.20, S * 0.32, S * 0.80, S * 0.70
    g.rounded_rectangle((x0, y0, x1, y1), S * 0.06, outline=color, width=w)
    k = S * 0.065
    for row, (y, n) in enumerate(((0.42, 5), (0.51, 5))):
        for i in range(n):
            cx = x0 + (x1 - x0) * (i + 0.5) / n
            g.rectangle((cx - k / 2, S * y - k / 2, cx + k / 2, S * y + k / 2), fill=color)
    g.rectangle((S * 0.34, S * 0.585, S * 0.66, S * 0.625), fill=color)


def icon(name, size, font_path):
    S = size * SS
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    g = ImageDraw.Draw(im)
    m = S * 0.04
    g.rounded_rectangle((m, m, S - m, S - m), S * 0.26, fill=GLASS + (230,),
                        outline=tuple(int(GLASS[i] + (CYAN[i] - GLASS[i]) * 0.45) for i in range(3)) + (255,),
                        width=max(SS, int(S * 0.03)))
    sign = SIGNS[name]
    if sign is None:
        keyboard(g, S, CYAN + (255,))
    else:
        font = ImageFont.truetype(font_path, int(S * (0.66 if size <= 22 else 0.58)))
        box = g.textbbox((0, 0), sign, font=font)
        w, h = box[2] - box[0], box[3] - box[1]
        g.text(((S - w) / 2 - box[0], (S - h) / 2 - box[1]), sign, font=font, fill=CYAN + (255,))
    return im.resize((size, size), Image.LANCZOS)


def main(root, font_path):
    base = os.path.join(root, "opt/trinity/share/icons/Syluxium")
    for size in SIZES:
        d = os.path.join(base, f"{size}x{size}/apps")
        os.makedirs(d, exist_ok=True)
        for name in SIGNS:
            icon(name, size, font_path).save(os.path.join(d, name + ".png"))
    print(f"  ICONS {len(SIGNS)} input method icons x {len(SIZES)} sizes")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "rootfs",
         sys.argv[2] if len(sys.argv) > 2 else "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc")
